ENTRY(switch_a3_heap_init)
SECTIONS
{
    . = 0x20e7dc;
    switch_a3_island_start = .;
    .text : { KEEP(*(.text.switch_a3_rx_entry)) *(.text*) }
    .rodata : { *(.rodata*) }
    .data : { *(.data*) *(.bss*) *(COMMON) }
    . = ALIGN(4);
    switch_a3_island_end = .;
    /DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) *(.comment*) *(.note*) }
    ASSERT(switch_a3_island_end - switch_a3_island_start <= 8192, "A3 payload exceeds8KiB")
}
