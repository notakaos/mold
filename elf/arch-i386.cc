#include "mold.h"

namespace mold::elf {

using E = I386;

static void write_plt_header(Context<E> &ctx, u8 *buf) {
  if (ctx.arg.pic) {
    static const u8 plt0[] = {
      0xff, 0xb3, 0, 0, 0, 0, // pushl GOTPLT+4(%ebx)
      0xff, 0xa3, 0, 0, 0, 0, // jmp *GOTPLT+8(%ebx)
      0x90, 0x90, 0x90, 0x90, // nop
    };
    memcpy(buf, plt0, sizeof(plt0));
    *(u32 *)(buf + 2) = ctx.gotplt->shdr.sh_addr - ctx.got->shdr.sh_addr + 4;
    *(u32 *)(buf + 8) = ctx.gotplt->shdr.sh_addr - ctx.got->shdr.sh_addr + 8;
  } else {
    static const u8 plt0[] = {
      0xff, 0x35, 0, 0, 0, 0, // pushl GOTPLT+4
      0xff, 0x25, 0, 0, 0, 0, // jmp *GOTPLT+8
      0x90, 0x90, 0x90, 0x90, // nop
    };
    memcpy(buf, plt0, sizeof(plt0));
    *(u32 *)(buf + 2) = ctx.gotplt->shdr.sh_addr + 4;
    *(u32 *)(buf + 8) = ctx.gotplt->shdr.sh_addr + 8;
  }
}

static void write_plt_entry(Context<E> &ctx, u8 *buf, Symbol<E> &sym,
                            i64 idx) {
  u8 *ent = buf + ctx.plt_hdr_size + sym.get_plt_idx(ctx) * ctx.plt_size;

  if (ctx.arg.pic) {
    static const u8 data[] = {
      0xff, 0xa3, 0, 0, 0, 0, // jmp *foo@GOT(%ebx)
      0x68, 0,    0, 0, 0,    // pushl $reloc_offset
      0xe9, 0,    0, 0, 0,    // jmp .PLT0@PC
    };
    memcpy(ent, data, sizeof(data));
    *(u32 *)(ent + 2) = sym.get_gotplt_addr(ctx) - ctx.got->shdr.sh_addr;
  } else {
    static const u8 data[] = {
      0xff, 0x25, 0, 0, 0, 0, // jmp *foo@GOT
      0x68, 0,    0, 0, 0,    // pushl $reloc_offset
      0xe9, 0,    0, 0, 0,    // jmp .PLT0@PC
    };
    memcpy(ent, data, sizeof(data));
    *(u32 *)(ent + 2) = sym.get_gotplt_addr(ctx);
  }

  *(u32 *)(ent + 7) = idx * sizeof(ElfRel<E>);
  *(u32 *)(ent + 12) = ctx.plt->shdr.sh_addr - sym.get_plt_addr(ctx) - 16;
}

template <>
void PltSection<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + this->shdr.sh_offset;
  write_plt_header(ctx, buf);

  for (i64 i = 0; i < symbols.size(); i++)
    write_plt_entry(ctx, buf, *symbols[i], i);
}

template <>
void PltGotSection<E>::copy_buf(Context<E> &ctx) {
  u8 *buf = ctx.buf + this->shdr.sh_offset;

  if (ctx.arg.pic) {
    static const u8 data[] = {
      0xff, 0xa3, 0, 0, 0, 0, // jmp   *foo@GOT(%ebx)
      0x66, 0x90,             // nop
    };

    for (i64 i = 0; i < symbols.size(); i++) {
      u8 *ent = buf + i * sizeof(data);
      memcpy(ent, data, sizeof(data));
      *(u32 *)(ent + 2) = symbols[i]->get_got_addr(ctx) - ctx.got->shdr.sh_addr;
    }
  } else {
    static const u8 data[] = {
      0xff, 0x25, 0, 0, 0, 0, // jmp   *foo@GOT
      0x66, 0x90,             // nop
    };

    for (i64 i = 0; i < symbols.size(); i++) {
      u8 *ent = buf + i * sizeof(data);
      memcpy(ent, data, sizeof(data));
      *(u32 *)(ent + 2) = symbols[i]->get_got_addr(ctx);
    }
  }
}

template <>
void EhFrameSection<E>::apply_reloc(Context<E> &ctx, ElfRel<E> &rel,
                                    u64 offset, u64 val) {
  u8 *loc = ctx.buf + this->shdr.sh_offset + offset;

  switch (rel.r_type) {
  case R_386_NONE:
    return;
  case R_386_32:
    *(u32 *)loc = val;
    return;
  case R_386_PC32:
    *(u32 *)loc = val - this->shdr.sh_addr - offset;
    return;
  }
  unreachable();
}

template <>
void InputSection<E>::apply_reloc_alloc(Context<E> &ctx, u8 *base) {
  ElfRel<E> *dynrel = nullptr;
  std::span<ElfRel<E>> rels = get_rels(ctx);
  i64 frag_idx = 0;

  if (ctx.reldyn)
    dynrel = (ElfRel<E> *)(ctx.buf + ctx.reldyn->shdr.sh_offset +
                              file.reldyn_offset + this->reldyn_offset);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_386_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    const SectionFragmentRef<E> *frag_ref = nullptr;
    if (rel_fragments && rel_fragments[frag_idx].idx == i)
      frag_ref = &rel_fragments[frag_idx++];

    auto overflow_check = [&](i64 val, i64 lo, i64 hi) {
      if (val < lo || hi <= val)
        Error(ctx) << *this << ": relocation " << rel << " against "
                   << sym << " out of range: " << val << " is not in ["
                   << lo << ", " << hi << ")";
    };

    auto write8 = [&](u64 val) {
      overflow_check(val, 0, 1 << 8);
      *loc = val;
    };

    auto write8s = [&](u64 val) {
      overflow_check(val, -(1 << 7), 1 << 7);
      *loc = val;
    };

    auto write16 = [&](u64 val) {
      overflow_check(val, 0, 1 << 16);
      *(u16 *)loc = val;
    };

    auto write16s = [&](u64 val) {
      overflow_check(val, -(1 << 15), 1 << 15);
      *(u16 *)loc = val;
    };

    auto write32 = [&](u64 val) {
      *(u32 *)loc = val;
    };

#define S   (frag_ref ? frag_ref->frag->get_addr(ctx) : sym.get_addr(ctx))
#define A   (frag_ref ? frag_ref->addend : this->get_addend(rel))
#define P   (output_section->shdr.sh_addr + offset + rel.r_offset)
#define G   (sym.get_got_addr(ctx) - ctx.got->shdr.sh_addr)
#define GOT ctx.got->shdr.sh_addr

    switch (rel.r_type) {
    case R_386_8:
      write8(S + A);
      continue;
    case R_386_16:
      write16(S + A);
      continue;
    case R_386_32:
      if (sym.is_absolute() || !ctx.arg.pic) {
        write32(S + A);
      } else if (sym.is_imported) {
        *dynrel++ = {P, R_386_32, (u32)sym.get_dynsym_idx(ctx)};
        write32(A);
      } else {
        if (!is_relr_reloc(ctx, rel))
          *dynrel++ = {P, R_386_RELATIVE, 0};
        write32(S + A);
      }
      continue;
    case R_386_PC8:
      write8s(S + A);
      continue;
    case R_386_PC16:
      write16s(S + A);
      continue;
    case R_386_PC32:
      if (sym.is_absolute() || !sym.is_imported || !ctx.arg.shared) {
        write32(S + A - P);
      } else {
        *dynrel++ = {P, R_386_32, (u32)sym.get_dynsym_idx(ctx)};
        write32(A);
      }
      continue;
    case R_386_PLT32:
      write32(S + A - P);
      continue;
    case R_386_GOT32:
    case R_386_GOT32X:
      write32(G + A);
      continue;
    case R_386_GOTOFF:
      write32(S + A - GOT);
      continue;
    case R_386_GOTPC:
      write32(GOT + A - P);
      continue;
    case R_386_TLS_GOTIE:
      write32(sym.get_gottp_addr(ctx) + A - GOT);
      continue;
    case R_386_TLS_LE:
      write32(S + A - ctx.tls_end);
      continue;
    case R_386_TLS_IE:
      write32(sym.get_gottp_addr(ctx) + A);
      continue;
    case R_386_TLS_GD:
      write32(sym.get_tlsgd_addr(ctx) + A - GOT);
      continue;
    case R_386_TLS_LDM:
      write32(ctx.got->get_tlsld_addr(ctx) + A - GOT);
      continue;
    case R_386_TLS_LDO_32:
      write32(S + A - ctx.tls_begin);
      continue;
    case R_386_SIZE32:
      write32(sym.esym().st_size + A);
      continue;
    case R_386_TLS_GOTDESC:
      if (sym.get_tlsdesc_idx(ctx) == -1) {
        static const u8 insn[] = {
          0x8d, 0x05, 0, 0, 0, 0, // lea 0, %eax
        };
        memcpy(loc - 2, insn, sizeof(insn));
        write32(S + A - ctx.tls_end);
      } else {
        write32(sym.get_tlsdesc_addr(ctx) + A - GOT);
      }
      continue;
    case R_386_TLS_DESC_CALL:
      if (ctx.arg.relax && !ctx.arg.shared) {
        // call *(%rax) -> nop
        loc[0] = 0x66;
        loc[1] = 0x90;
      }
      continue;
    default:
      unreachable();
    }

#undef S
#undef A
#undef P
#undef G
#undef GOT
  }
}

template <>
void InputSection<E>::apply_reloc_nonalloc(Context<E> &ctx, u8 *base) {
  std::span<ElfRel<E>> rels = get_rels(ctx);

  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_386_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];
    u8 *loc = base + rel.r_offset;

    if (!sym.file) {
      report_undef(ctx, file, sym);
      continue;
    }

    SectionFragment<E> *frag;
    i64 addend;
    std::tie(frag, addend) = get_fragment(ctx, rel);

    auto overflow_check = [&](i64 val, i64 lo, i64 hi) {
      if (val < lo || hi <= val)
        Error(ctx) << *this << ": relocation " << rel << " against "
                   << sym << " out of range: " << val << " is not in ["
                   << lo << ", " << hi << ")";
    };

    auto write8 = [&](u64 val) {
      overflow_check(val, 0, 1 << 8);
      *loc = val;
    };

    auto write8s = [&](u64 val) {
      overflow_check(val, -(1 << 7), 1 << 7);
      *loc = val;
    };

    auto write16 = [&](u64 val) {
      overflow_check(val, 0, 1 << 16);
      *(u16 *)loc = val;
    };

    auto write16s = [&](u64 val) {
      overflow_check(val, -(1 << 15), 1 << 15);
      *(u16 *)loc = val;
    };

#define S   (frag ? frag->get_addr(ctx) : sym.get_addr(ctx))
#define A   (frag ? addend : this->get_addend(rel))
#define G   (sym.get_got_addr(ctx) - ctx.got->shdr.sh_addr)
#define GOT ctx.got->shdr.sh_addr

    switch (rel.r_type) {
    case R_386_8:
      write8(S + A);
      continue;
    case R_386_16:
      write16(S + A);
      continue;
    case R_386_32:
      *(u32 *)loc = S + A;
      continue;
    case R_386_PC8:
      write8s(S + A);
      continue;
    case R_386_PC16:
      write16s(S + A);
      continue;
    case R_386_PC32:
      *(u32 *)loc = S + A;
      continue;
    case R_386_GOTPC:
      *(u32 *)loc = GOT + A;
      continue;
    case R_386_GOTOFF:
      *(u32 *)loc = S + A - GOT;
      continue;
    case R_386_TLS_LDO_32:
      *(u32 *)loc = S + A - ctx.tls_begin;
      continue;
    case R_386_SIZE32:
      *(u32 *)loc = sym.esym().st_size + A;
      continue;
    default:
      unreachable();
    }

#undef S
#undef A
#undef GOT
  }
}

template <>
void InputSection<E>::scan_relocations(Context<E> &ctx) {
  assert(shdr().sh_flags & SHF_ALLOC);

  this->reldyn_offset = file.num_dynrel * sizeof(ElfRel<E>);
  std::span<ElfRel<E>> rels = get_rels(ctx);

  // Scan relocations
  for (i64 i = 0; i < rels.size(); i++) {
    const ElfRel<E> &rel = rels[i];
    if (rel.r_type == R_386_NONE)
      continue;

    Symbol<E> &sym = *file.symbols[rel.r_sym];

    if (!sym.file) {
      report_undef(ctx, file, sym);
      continue;
    }

    if (sym.get_type() == STT_GNU_IFUNC) {
      sym.flags |= NEEDS_GOT;
      sym.flags |= NEEDS_PLT;
    }

    switch (rel.r_type) {
    case R_386_8:
    case R_386_16: {
      Action table[][4] = {
        // Absolute  Local  Imported data  Imported code
        {  NONE,     ERROR, ERROR,         ERROR },      // DSO
        {  NONE,     ERROR, ERROR,         ERROR },      // PIE
        {  NONE,     NONE,  COPYREL,       PLT   },      // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_386_32: {
      Action table[][4] = {
        // Absolute  Local    Imported data  Imported code
        {  NONE,     BASEREL, DYNREL,        DYNREL },     // DSO
        {  NONE,     BASEREL, DYNREL,        DYNREL },     // PIE
        {  NONE,     NONE,    COPYREL,       PLT    },     // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_386_PC8:
    case R_386_PC16: {
      Action table[][4] = {
        // Absolute  Local  Imported data  Imported code
        {  ERROR,    NONE,  ERROR,         ERROR },      // DSO
        {  ERROR,    NONE,  COPYREL,       PLT   },      // PIE
        {  NONE,     NONE,  COPYREL,       PLT   },      // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_386_PC32: {
      Action table[][4] = {
        // Absolute  Local  Imported data  Imported code
        {  ERROR,    NONE,  DYNREL,        DYNREL },     // DSO
        {  ERROR,    NONE,  COPYREL,       PLT    },     // PIE
        {  NONE,     NONE,  COPYREL,       PLT    },     // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_386_GOT32:
    case R_386_GOT32X:
    case R_386_GOTPC:
      sym.flags |= NEEDS_GOT;
      break;
    case R_386_PLT32: {
      Action table[][4] = {
        // Absolute  Local  Imported data  Imported code
        {  NONE,     NONE,  PLT,           PLT    },     // DSO
        {  NONE,     NONE,  PLT,           PLT    },     // PIE
        {  NONE,     NONE,  PLT,           PLT    },     // PDE
      };
      dispatch(ctx, table, i, rel, sym);
      break;
    }
    case R_386_TLS_GOTIE:
    case R_386_TLS_LE:
    case R_386_TLS_IE:
      sym.flags |= NEEDS_GOTTP;
      break;
    case R_386_TLS_GD:
      sym.flags |= NEEDS_TLSGD;
      break;
    case R_386_TLS_LDM:
      ctx.needs_tlsld = true;
      break;
    case R_386_TLS_GOTDESC:
      if (!ctx.arg.relax || ctx.arg.shared)
        sym.flags |= NEEDS_TLSDESC;
      break;
    case R_386_GOTOFF:
    case R_386_TLS_LDO_32:
    case R_386_SIZE32:
    case R_386_TLS_DESC_CALL:
      break;
    default:
      Error(ctx) << *this << ": unknown relocation: " << rel;
    }
  }
}

} // namespace mold::elf
