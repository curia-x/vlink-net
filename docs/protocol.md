# vlink frame protocol v1

All backend transports carry vlink frames.

```c
struct vlink_hdr {
    u8 magic[2];        /* 'V', 'L' */
    u8 version;         /* 1 */
    u8 type;            /* DATA = 1 */
    __le16 hdr_len;     /* sizeof(struct vlink_hdr) */
    __le16 payload_len; /* Ethernet frame length */
    __le32 flags;       /* reserved */
    __le32 crc32;       /* crc32_le(~0, payload, payload_len) */
} __packed;
```

Current version only supports DATA frames. HELLO/KEEPALIVE/RESET are intentionally left out of
this first version to keep the project focused on functional completeness rather than advanced features.
