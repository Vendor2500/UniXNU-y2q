import pathlib, sys

PATH = "src/freedreno/vulkan/tu_clear_blit.cc"
EDITS = [
    ("static inline uint32_t\n"
     "tu_gmem_transfer_swap_fixup(uint32_t info, uint32_t swap_mask, uint32_t identity, bool mutable_view)\n"
     "{\n"
     "   return mutable_view ? info : ((info & ~swap_mask) | identity);\n"
     "}\n",
     "template <chip CHIP>\n"
     "static inline uint32_t\n"
     "tu_gmem_transfer_swap_fixup(uint32_t info, uint32_t swap_mask, uint32_t identity, bool mutable_view)\n"
     "{\n"
     "   /* A6xx has no MUTABLEEN and its CCU ignores the MRT swap in tile memory, which holds WZYX: the memory\n"
     "    * side keeps the image's own swap there, as upstream (store_cp_blit). */\n"
     "   return (CHIP < A7XX || mutable_view) ? info : ((info & ~swap_mask) | identity);\n"
     "}\n"),
    ("dst_info = tu_gmem_transfer_swap_fixup(", "dst_info = tu_gmem_transfer_swap_fixup<CHIP>("),
    ("mrt_buf_info = tu_gmem_transfer_swap_fixup(", "mrt_buf_info = tu_gmem_transfer_swap_fixup<CHIP>("),
    ("RB_MRT_BUF_INFO = tu_gmem_transfer_swap_fixup(", "RB_MRT_BUF_INFO = tu_gmem_transfer_swap_fixup<CHIP>("),
    ("tu_gmem_transfer_swap_fixup(blt_view->view->RB_RESOLVE_SYSTEM_BUFFER_INFO,",
     "tu_gmem_transfer_swap_fixup<CHIP>(blt_view->view->RB_RESOLVE_SYSTEM_BUFFER_INFO,"),
]

def apply(root):
    f = pathlib.Path(root) / PATH
    s = f.read_bytes().decode()
    for old, new in EDITS:
        n = s.count(old)
        if n != 1:
            sys.exit(f"{PATH}: expected 1 match, found {n}: {old[:60]!r}")
        s = s.replace(old, new)
    assert "tu_gmem_transfer_swap_fixup(" not in s.replace("tu_gmem_transfer_swap_fixup(uint32_t", ""), "unconverted call site"
    f.write_bytes(s.encode())

if __name__ == "__main__":
    apply(sys.argv[1])
