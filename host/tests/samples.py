"""Synthetic but self-consistent SPD images for the tests."""

from spdtool.image import SpdImage


def ddr4_3200_8gb() -> SpdImage:
    """DDR4-3200 UDIMM, 8 GB, 1Rx8, 8 Gb dies, CL22-22-22."""
    d = bytearray(512)
    d[0:4] = bytes([0x23, 0x11, 0x0C, 0x02])
    d[4] = 0x85  # 8 Gb, 4 bank groups, 4 banks/group
    d[5] = 0x21  # 16 rows, 10 columns
    d[12] = 0x01  # 1 package rank, x8
    d[13] = 0x03  # 64-bit, no ECC
    d[18] = 5  # tCKmin 625 ps
    d[19] = 13  # tCKmax 1625 ps
    cl = sum(1 << (c - 7) for c in range(10, 25))
    d[20:24] = cl.to_bytes(4, "little")
    d[24] = d[25] = d[26] = 110  # tAA/tRCD/tRP 13.75 ns
    d[27] = 0x11  # tRC/tRAS upper nibbles
    d[28] = 0x00  # tRAS 256 MTB = 32 ns
    d[29] = 110  # tRC 366 MTB = 45.75 ns
    d[30:32] = (2800).to_bytes(2, "little")  # tRFC1 350 ns
    d[32:34] = (2080).to_bytes(2, "little")  # tRFC2 260 ns
    d[34:36] = (1280).to_bytes(2, "little")  # tRFC4 160 ns
    d[320:322] = bytes([0x80, 0xCE])  # Samsung
    d[323:325] = bytes([0x21, 0x15])  # 2021 week 15
    d[325:329] = bytes.fromhex("01020304")
    d[329:349] = b"TEST-DDR4-3200".ljust(20, b" ")
    img = SpdImage(bytes(d))
    img.fix_crc()
    return img


def ddr5_4800_16gb() -> SpdImage:
    """DDR5-4800 UDIMM, 16 GB, 1Rx8, 16 Gb dies, CL40-39-39."""
    d = bytearray(1024)
    d[0:4] = bytes([0x30, 0x10, 0x12, 0x02])
    d[4] = 0x04  # 16 Gb, monolithic
    d[5] = 0x00  # 16 rows, 10 columns
    d[6] = 0x20  # x8
    d[7] = 0x62  # 8 bank groups, 4 banks/group
    d[20:22] = (416).to_bytes(2, "little")
    d[22:24] = (1010).to_bytes(2, "little")
    cl = sum(1 << ((c - 20) // 2) for c in range(22, 42, 2))
    d[24:29] = cl.to_bytes(5, "little")
    for off, ps in ((30, 16000), (32, 16000), (34, 16000), (36, 32000), (38, 48000), (40, 30000)):
        d[off : off + 2] = ps.to_bytes(2, "little")
    for off, ns in ((42, 295), (44, 160), (46, 130)):
        d[off : off + 2] = ns.to_bytes(2, "little")
    d[194:197] = bytes([0x80, 0xCE, 0x80])  # hub: Samsung, SPD5118, installed
    d[234] = 0x00  # 1 package rank per sub-channel
    d[235] = 0x22  # 2 sub-channels x 32 bit
    d[512:514] = bytes([0x80, 0xAD])  # SK hynix
    d[515:517] = bytes([0x23, 0x40])
    d[517:521] = bytes.fromhex("a1b2c3d4")
    d[521:551] = b"TEST-DDR5-4800".ljust(30, b" ")
    img = SpdImage(bytes(d))
    img.fix_crc()
    return img
