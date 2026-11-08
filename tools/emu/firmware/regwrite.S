// hekate's last steps on CCPLEX core 0 at EL3: the 32-bit register writes of its table (address, value; address 0
// ends it), then a jump to the 64-bit entry after the end marker. hekate.py builds the table behind this code.
    .text
    .global _start
_start:
    adr x0, table
1:  ldp w1, w2, [x0], #8
    cbz w1, 2f
    str w2, [x1]
    b 1b
2:  dsb sy
    isb
    ldr x1, [x0]
    br x1
    .p2align 3
table:
