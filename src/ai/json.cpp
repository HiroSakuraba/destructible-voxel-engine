#include "dve/ai/json.hpp"

#include <charconv>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace dve::ai {

bool JsonValue::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(value_); }
bool JsonValue::is_bool() const noexcept { return std::holds_alternative<bool>(value_); }
bool JsonValue::is_number() const noexcept { return std::holds_alternative<double>(value_); }
bool JsonValue::is_string() const noexcept { return std::holds_alternative<std::string>(value_); }
bool JsonValue::is_array() const noexcept { return std::holds_alternative<Array>(value_); }
bool JsonValue::is_object() const noexcept { return std::holds_alternative<Object>(value_); }
bool JsonValue::as_bool(bool fallback) const noexcept { const auto* v=std::get_if<bool>(&value_); return v?*v:fallback; }
double JsonValue::as_number(double fallback) const noexcept { const auto* v=std::get_if<double>(&value_); return v?*v:fallback; }
std::string_view JsonValue::as_string(std::string_view fallback) const noexcept { const auto* v=std::get_if<std::string>(&value_); return v?std::string_view(*v):fallback; }
const JsonValue::Array& JsonValue::as_array() const { return std::get<Array>(value_); }
JsonValue::Array& JsonValue::as_array() { return std::get<Array>(value_); }
const JsonValue::Object& JsonValue::as_object() const { return std::get<Object>(value_); }
JsonValue::Object& JsonValue::as_object() { return std::get<Object>(value_); }
const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    const auto* object=std::get_if<Object>(&value_); if(!object) return nullptr;
    const auto it=object->find(key); return it==object->end()?nullptr:&it->second;
}
JsonValue* JsonValue::find(std::string_view key) noexcept {
    auto* object=std::get_if<Object>(&value_); if(!object) return nullptr;
    const auto it=object->find(key); return it==object->end()?nullptr:&it->second;
}
JsonValue& JsonValue::operator[](std::string key) {
    if(!is_object()) value_=Object{};
    return std::get<Object>(value_)[std::move(key)];
}

namespace {
class Parser {
public:
    explicit Parser(std::string_view text):text_(text){}
    JsonParseResult run(){
        JsonParseResult out;
        try { skip(); out.value=parse_value(); skip(); if(pos_!=text_.size()) fail("trailing characters"); }
        catch(const std::runtime_error& e){out.value.reset();out.error=e.what();out.errorOffset=pos_;}
        return out;
    }
private:
    std::string_view text_; std::size_t pos_{};
    [[noreturn]] void fail(std::string_view m) const { throw std::runtime_error(std::string(m)); }
    void skip(){while(pos_<text_.size()&&(text_[pos_]==' '||text_[pos_]=='\n'||text_[pos_]=='\r'||text_[pos_]=='\t'))++pos_;}
    char take(){if(pos_>=text_.size()) fail("unexpected end of JSON");return text_[pos_++];}
    bool consume(std::string_view s){if(text_.substr(pos_,s.size())!=s)return false;pos_+=s.size();return true;}
    JsonValue parse_value(){
        skip(); if(pos_>=text_.size()) fail("missing JSON value");
        const char c=text_[pos_];
        if(c=='{') return parse_object();
        if(c=='[') return parse_array();
        if(c=='\"') return JsonValue(parse_string());
        if(c=='t'){if(!consume("true"))fail("invalid literal");return JsonValue(true);} if(c=='f'){if(!consume("false"))fail("invalid literal");return JsonValue(false);}
        if(c=='n'){if(!consume("null"))fail("invalid literal");return JsonValue(nullptr);} if(c=='-'||(c>='0'&&c<='9'))return JsonValue(parse_number());
        fail("invalid JSON value");
    }
    std::string parse_string(){
        if(take()!='\"') fail("expected string");
        std::string out;
        while(pos_<text_.size()){
            const char c=take(); if(c=='\"')return out; if(static_cast<unsigned char>(c)<0x20)fail("control character in string");
            if(c!='\\'){out.push_back(c);continue;} const char e=take();
            switch(e){case '\"':out.push_back('\"');break;case '\\':out.push_back('\\');break;case '/':out.push_back('/');break;case 'b':out.push_back('\b');break;case 'f':out.push_back('\f');break;case 'n':out.push_back('\n');break;case 'r':out.push_back('\r');break;case 't':out.push_back('\t');break;
            case 'u':{unsigned code=0;for(int i=0;i<4;++i){const char h=take();code<<=4;if(h>='0'&&h<='9')code|=unsigned(h-'0');else if(h>='a'&&h<='f')code|=unsigned(h-'a'+10);else if(h>='A'&&h<='F')code|=unsigned(h-'A'+10);else fail("invalid unicode escape");}
                if(code<=0x7f)out.push_back(static_cast<char>(code));else if(code<=0x7ff){out.push_back(static_cast<char>(0xc0|(code>>6)));out.push_back(static_cast<char>(0x80|(code&0x3f)));}else{out.push_back(static_cast<char>(0xe0|(code>>12)));out.push_back(static_cast<char>(0x80|((code>>6)&0x3f)));out.push_back(static_cast<char>(0x80|(code&0x3f)));}break;}
            default:fail("invalid string escape");}
        } fail("unterminated string");
    }
    double parse_number(){
        const std::size_t begin=pos_; if(text_[pos_]=='-')++pos_; if(pos_>=text_.size())fail("invalid number");
        if(text_[pos_]=='0')++pos_;else{if(text_[pos_]<'1'||text_[pos_]>'9')fail("invalid number");while(pos_<text_.size()&&text_[pos_]>='0'&&text_[pos_]<='9')++pos_;}
        if(pos_<text_.size()&&text_[pos_]=='.'){++pos_;if(pos_>=text_.size()||text_[pos_]<'0'||text_[pos_]>'9')fail("invalid fraction");while(pos_<text_.size()&&text_[pos_]>='0'&&text_[pos_]<='9')++pos_;}
        if(pos_<text_.size()&&(text_[pos_]=='e'||text_[pos_]=='E')){++pos_;if(pos_<text_.size()&&(text_[pos_]=='+'||text_[pos_]=='-'))++pos_;if(pos_>=text_.size()||text_[pos_]<'0'||text_[pos_]>'9')fail("invalid exponent");while(pos_<text_.size()&&text_[pos_]>='0'&&text_[pos_]<='9')++pos_;}
        double value{}; const auto s=text_.substr(begin,pos_-begin); const auto result=std::from_chars(s.data(),s.data()+s.size(),value);
        if(result.ec!=std::errc{}||!std::isfinite(value)) fail("invalid finite number");
        return value;
    }
    JsonValue parse_array(){take();JsonValue::Array a;skip();if(pos_<text_.size()&&text_[pos_]==']'){++pos_;return a;}while(true){a.push_back(parse_value());skip();const char c=take();if(c==']')break;if(c!=',')fail("expected comma in array");skip();}return a;}
    JsonValue parse_object(){take();JsonValue::Object o;skip();if(pos_<text_.size()&&text_[pos_]=='}'){++pos_;return o;}while(true){skip();if(pos_>=text_.size()||text_[pos_]!='\"')fail("expected object key");std::string k=parse_string();skip();if(take()!=':')fail("expected colon");skip();if(!o.emplace(std::move(k),parse_value()).second)fail("duplicate object key");skip();const char c=take();if(c=='}')break;if(c!=',')fail("expected comma in object");skip();}return o;}
};
void emit(const JsonValue& v,std::string& out,bool pretty,int depth){
    const auto indent=[&](int n){if(pretty)out.append(static_cast<std::size_t>(n)*2,' ');};
    if(v.is_null()){out+="null";return;} if(v.is_bool()){out+=v.as_bool()?"true":"false";return;} if(v.is_number()){std::ostringstream s;s<<std::setprecision(17)<<v.as_number();out+=s.str();return;} if(v.is_string()){out+='\"';out+=json_escape(v.as_string());out+='\"';return;}
    if(v.is_array()){out+='[';const auto& a=v.as_array();for(std::size_t i=0;i<a.size();++i){if(i)out+=',';if(pretty){out+='\n';indent(depth+1);}emit(a[i],out,pretty,depth+1);}if(pretty&&!a.empty()){out+='\n';indent(depth);}out+=']';return;}
    out+='{';const auto& o=v.as_object();std::size_t i=0;for(const auto& [k,x]:o){if(i++)out+=',';if(pretty){out+='\n';indent(depth+1);}out+='\"';out+=json_escape(k);out+='\"';out+=pretty?": ":":";emit(x,out,pretty,depth+1);}if(pretty&&!o.empty()){out+='\n';indent(depth);}out+='}';
}
}
JsonParseResult parse_json(std::string_view text){return Parser(text).run();}
std::string stringify_json(const JsonValue& value,bool pretty){std::string out;emit(value,out,pretty,0);return out;}
std::string json_escape(std::string_view text){std::string out;for(unsigned char c:text){switch(c){case '\"':out+="\\\"";break;case '\\':out+="\\\\";break;case '\b':out+="\\b";break;case '\f':out+="\\f";break;case '\n':out+="\\n";break;case '\r':out+="\\r";break;case '\t':out+="\\t";break;default:if(c<0x20){static constexpr char h[]="0123456789abcdef";out+="\\u00";out.push_back(h[c>>4]);out.push_back(h[c&15]);}else out.push_back(static_cast<char>(c));}}return out;}
} // namespace dve::ai
