# Current brick uploads and generation changes

An existing brick filled locally retains its identity and increments its generation. A
`replace_brick` snapshot remains authoritative and may intentionally assign a lower number,
as happens during network repair.

`PackedBrickmapScene::update(object, edits)` first checks edit generation equality and then
publishes uploads built from that current object, even if the scene previously held a larger
generation. The public `publish_uploads` path keeps its monotonic same-source input contract
and rejects older external uploads.

The fill and lower-generation repair assertions failed on `main`. The core brickmap test and
network runtime tests passed after the fix.
