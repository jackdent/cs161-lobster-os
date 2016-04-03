#define PAGE_TABLE_SIZE (1 << 10)

#define L1_PT_MASK(va) (va >> 22)
#define L2_PT_MASK(va) ((va >> 12) & 0x3FF)
