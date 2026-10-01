#pragma once
#include "ring.hpp"
#include "runtime.hpp"
namespace vinc::bus::detail {
enum Operation : std::uint16_t { Advertise = 1, Subscribe = 2, ClosePublisher = 3, Unsubscribe = 4, Attach = 5, Detach = 6 };
inline void encode_descriptor(control::Bytes& b, const Descriptor& d) {
    control::put_u64(b, d.id); control::put_u64(b, d.owner); control::put_u64(b, d.schema); control::put_u64(b, d.rank);
    control::put_u32(b, d.slots); control::put_u32(b, d.readers); control::put_u64(b, d.bytes);
    control::put_string(b, d.control); control::put_string(b, d.data); control::put_string(b, d.counter); control::put_string(b, d.publisher);
}
inline Descriptor decode_descriptor(control::Reader& r) {
    Descriptor d; d.id = r.u64(); d.owner = r.u64(); d.schema = r.u64(); d.rank = r.u64();
    d.slots = r.u32(); d.readers = r.u32(); d.bytes = r.u64();
    d.control = r.string(); d.data = r.string(); d.counter = r.string(); d.publisher = r.string(); return d;
}
inline void encode_attachment(control::Bytes& b, const Attachment& a) {
    encode_descriptor(b, a.descriptor); control::put_u32(b, a.reader); control::put_u64(b, a.floor);
}
inline Attachment decode_attachment(control::Reader& r) {
    Attachment a; a.descriptor = decode_descriptor(r); a.reader = r.u32(); a.floor = r.u64(); return a;
}
}
