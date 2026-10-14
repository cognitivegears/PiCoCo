# SN74LVC245A at 3.3 V for every bus buffer

The first design used auto-direction level shifters (TXB0108 / TXS0108E) and a 74FST3257 mux on the CoCo bus; their weak auto-sensing drivers and mux confusion were the reason for the v2 redesign. All four buffers (U10 data, U11/U12 address, U13 control) are SN74LVC245A with direction hardwired from the buffered /R/W; do not reintroduce an auto-sensing part. Rationale: `docs/hardware-design.md` §4 and bigmessowires.com/2023/08/22/a-tale-of-three-bidirectional-level-shifters/.
