/*
 * ELF object format helpers
 *
 *  Copyright (C) 2003  Michael Urman
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND OTHER CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR OTHER CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#include <util.h>
/*@unused@*/ RCSID("$Id$");

#define YASM_LIB_INTERNAL
#define YASM_EXPR_INTERNAL
#include <libyasm.h>
#define YASM_OBJFMT_ELF_INTERNAL
#include "elf.h"

#define YASM_WRITE_32I_L(p, i) do {\
    assert(yasm_intnum_check_size(i, 32, 0, 2)); \
    yasm_intnum_get_sized(i, p, 4, 32, 0, 0, 0, 0); \
    p += 4; } while (0)

#define YASM_WRITE_64I_L(p, i) do {\
    assert(yasm_intnum_check_size(i, 64, 0, 2)); \
    yasm_intnum_get_sized(i, p, 8, 64, 0, 0, 0, 0); \
    p += 8; } while (0)

#define YASM_WRITE_64C_L(p, hi, lo) do {\
    YASM_WRITE_32_L(p, lo); \
    YASM_WRITE_32_L(p, hi); } while (0)

#define YASM_WRITE_64Z_L(p, i)		YASM_WRITE_64C_L(p, 0, i)

static void elf_section_data_destroy(void *data);
static void elf_secthead_print(void *data, FILE *f, int indent_level);

const yasm_assoc_data_callback elf_section_data = {
    elf_section_data_destroy,
    elf_secthead_print
};

static void elf_symrec_data_destroy(/*@only@*/ void *d);
static void elf_symtab_entry_print(void *data, FILE *f, int indent_level);

const yasm_assoc_data_callback elf_symrec_data = {
    elf_symrec_data_destroy,
    elf_symtab_entry_print
};

typedef int(*func_accepts_size_t)(size_t);
typedef void(*func_write_symtab_entry)(unsigned char *bufp,
                                       elf_symtab_entry *entry,
                                       yasm_intnum *value_intn,
                                       yasm_intnum *size_intn);
typedef void(*func_write_secthead)(unsigned char *bufp, elf_secthead *shead);
typedef void(*func_write_secthead_rel)(unsigned char *bufp,
                                       elf_secthead *shead,
                                       elf_section_index symtab_idx,
                                       elf_section_index sindex);

typedef unsigned char(*func_map_reloc_info_to_type)(elf_reloc_entry *reloc);
typedef void(*func_write_reloc)(unsigned char *bufp,
                                elf_reloc_entry *reloc,
                                unsigned char r_type,
                                unsigned char r_sym);
typedef void (*func_write_proghead)(unsigned char **bufpp,
                                    elf_offset secthead_addr,
                                    unsigned long secthead_count,
                                    elf_section_index shstrtab_index);

typedef struct {
    const char *arch;
    const char *machine;
    const unsigned long symtab_entry_size;
    const unsigned long symtab_entry_align;
    const unsigned long reloc_entry_size;
    const unsigned long secthead_size;
    const unsigned long proghead_size;
    func_accepts_size_t accepts_reloc_size;
    func_write_symtab_entry write_symtab_entry;
    func_write_secthead write_secthead;
    func_write_secthead_rel write_secthead_rel;
    func_map_reloc_info_to_type map_reloc_info_to_type;
    func_write_reloc write_reloc;
    func_write_proghead write_proghead;
} elf_machine_handler;

static int elf_accept_32(size_t val)
{
    return val == 32;
}
static int elf_accept_8_16_32_64(size_t val)
{
    return (val&(val-1)) ? 0 : ((val & (8|16|32|64)) != 0);
}

static void elf_write_symtab_entry_x86(unsigned char *bufp,
                                       elf_symtab_entry *entry,
                                       yasm_intnum *value_intn,
                                       yasm_intnum *size_intn)
{
    YASM_WRITE_32_L(bufp, entry->name ? entry->name->index : 0);
    YASM_WRITE_32I_L(bufp, value_intn);
    YASM_WRITE_32I_L(bufp, size_intn);

    YASM_WRITE_8(bufp, ELF32_ST_INFO(entry->bind, entry->type));
    YASM_WRITE_8(bufp, 0);
    if (entry->sect) {
        if (yasm_section_is_absolute(entry->sect)) {
            YASM_WRITE_16_L(bufp, SHN_ABS);
        } else {
            elf_secthead *shead = yasm_section_get_data(entry->sect,
                &elf_section_data);
            if (!shead)
                yasm_internal_error(
                    N_("symbol references section without data"));
            YASM_WRITE_16_L(bufp, shead->index);
        }
    } else {
        YASM_WRITE_16_L(bufp, entry->index);
    }
}

static void elf_write_symtab_entry_x86_64(unsigned char *bufp,
                                       elf_symtab_entry *entry,
                                       yasm_intnum *value_intn,
                                       yasm_intnum *size_intn)
{
    YASM_WRITE_32_L(bufp, entry->name ? entry->name->index : 0);
    YASM_WRITE_8(bufp, ELF64_ST_INFO(entry->bind, entry->type));
    YASM_WRITE_8(bufp, 0);
    if (entry->sect) {
        if (yasm_section_is_absolute(entry->sect)) {
            YASM_WRITE_16_L(bufp, SHN_ABS);
        } else {
            elf_secthead *shead = yasm_section_get_data(entry->sect,
                &elf_section_data);
            if (!shead)
                yasm_internal_error(
                    N_("symbol references section without data"));
            YASM_WRITE_16_L(bufp, shead->index);
        }
    } else {
        YASM_WRITE_16_L(bufp, entry->index);
    }
    YASM_WRITE_64I_L(bufp, value_intn);
    YASM_WRITE_64I_L(bufp, size_intn);
}

static void elf_write_secthead_x86(unsigned char *bufp, elf_secthead *shead)
{
    YASM_WRITE_32_L(bufp, shead->name ? shead->name->index : 0);
    YASM_WRITE_32_L(bufp, shead->type);
    YASM_WRITE_32_L(bufp, shead->flags);
    YASM_WRITE_32_L(bufp, 0); /* vmem address */

    YASM_WRITE_32_L(bufp, shead->offset);
    YASM_WRITE_32I_L(bufp, shead->size);
    YASM_WRITE_32_L(bufp, shead->link);
    YASM_WRITE_32_L(bufp, shead->info);

    if (shead->align)
        YASM_WRITE_32I_L(bufp, shead->align);
    else
        YASM_WRITE_32_L(bufp, 0);
    YASM_WRITE_32_L(bufp, shead->entsize);

}

static void elf_write_secthead_x86_64(unsigned char *bufp, elf_secthead *shead)
{
    YASM_WRITE_32_L(bufp, shead->name ? shead->name->index : 0);
    YASM_WRITE_32_L(bufp, shead->type);
    YASM_WRITE_64Z_L(bufp, shead->flags);
    YASM_WRITE_64Z_L(bufp, 0);		/* vmem address */
    YASM_WRITE_64Z_L(bufp, shead->offset);
    YASM_WRITE_64I_L(bufp, shead->size);

    YASM_WRITE_32_L(bufp, shead->link);
    YASM_WRITE_32_L(bufp, shead->info);

    if (shead->align)
        YASM_WRITE_64I_L(bufp, shead->align);
    else
        YASM_WRITE_64Z_L(bufp, 0);
    YASM_WRITE_64Z_L(bufp, shead->entsize);
    
}

static void elf_write_secthead_rel_x86(unsigned char *bufp,
                                       elf_secthead *shead,
                                       elf_section_index symtab_idx,
                                       elf_section_index sindex)
{
    YASM_WRITE_32_L(bufp, shead->rel_name ? shead->rel_name->index : 0);
    YASM_WRITE_32_L(bufp, SHT_REL);
    YASM_WRITE_32_L(bufp, 0);
    YASM_WRITE_32_L(bufp, 0);

    YASM_WRITE_32_L(bufp, shead->rel_offset);
    YASM_WRITE_32_L(bufp, RELOC32_SIZE * shead->nreloc);/* size */
    YASM_WRITE_32_L(bufp, symtab_idx);		/* link: symtab index */
    YASM_WRITE_32_L(bufp, shead->index);	/* info: relocated's index */

    YASM_WRITE_32_L(bufp, RELOC32_ALIGN);	/* align */
    YASM_WRITE_32_L(bufp, RELOC32_SIZE);	/* entity size */
}

static void elf_write_secthead_rel_x86_64(unsigned char *bufp,
                                          elf_secthead *shead,
                                          elf_section_index symtab_idx,
                                          elf_section_index sindex)
{
    yasm_intnum *nreloc;
    yasm_intnum *relocsize;

    YASM_WRITE_32_L(bufp, shead->rel_name ? shead->rel_name->index : 0);
    YASM_WRITE_32_L(bufp, SHT_REL);
    YASM_WRITE_64Z_L(bufp, 0);
    YASM_WRITE_64Z_L(bufp, 0);
    YASM_WRITE_64Z_L(bufp, shead->rel_offset);

    nreloc = yasm_intnum_create_uint(shead->nreloc);
    relocsize = yasm_intnum_create_uint(RELOC64_SIZE);
    yasm_intnum_calc(relocsize, YASM_EXPR_MUL, nreloc, 0);
    YASM_WRITE_64I_L(bufp, relocsize);		/* size */
    yasm_intnum_destroy(nreloc);
    yasm_intnum_destroy(relocsize);

    YASM_WRITE_32_L(bufp, symtab_idx);		/* link: symtab index */
    YASM_WRITE_32_L(bufp, shead->index);	/* info: relocated's index */
    YASM_WRITE_64Z_L(bufp, RELOC64_ALIGN);	/* align */
    YASM_WRITE_64Z_L(bufp, RELOC64_SIZE);	/* entity size */
}

static unsigned char elf_map_reloc_info_to_type_x86(elf_reloc_entry *reloc)
{
    return (unsigned char)(reloc->rtype_rel ? R_386_PC32 : R_386_32);
}

static unsigned char elf_map_reloc_info_to_type_x86_64(elf_reloc_entry *reloc)
{
    if (reloc->rtype_rel) {
        switch (reloc->valsize) {
            case 8: return (unsigned char) R_X86_64_PC8;
            case 16: return (unsigned char) R_X86_64_PC16;
            case 32: return (unsigned char) R_X86_64_PC32;
            default: yasm_internal_error(N_("Unsupported relocation size"));
        }
    } else {
        switch (reloc->valsize) {
            case 8: return (unsigned char) R_X86_64_8;
            case 16: return (unsigned char) R_X86_64_16;
            case 32: return (unsigned char) R_X86_64_32;
            case 64: return (unsigned char) R_X86_64_64;
            default: yasm_internal_error(N_("Unsupported relocation size"));
        }
    }
    return 0;
}

static void elf_write_reloc_x86(unsigned char *bufp, elf_reloc_entry *reloc,
                                unsigned char r_type, unsigned char r_sym)
{
    YASM_WRITE_32I_L(bufp, reloc->reloc.addr);
    YASM_WRITE_32_L(bufp, ELF32_R_INFO(r_sym, r_type));
}

static void elf_write_reloc_x86_64(unsigned char *bufp, elf_reloc_entry *reloc,
                                   unsigned char r_type, unsigned char r_sym)
{
    YASM_WRITE_64I_L(bufp, reloc->reloc.addr);
    /*YASM_WRITE_64_L(bufp, ELF64_R_INFO(r_sym, r_type));*/
    YASM_WRITE_64C_L(bufp, r_sym, r_type);
}

static void elf_write_proghead_x86(unsigned char **bufpp,
                                   elf_offset secthead_addr,
                                   unsigned long secthead_count,
                                   elf_section_index shstrtab_index)
{
    unsigned char *bufp = *bufpp;
    unsigned char *buf = bufp-4;
    YASM_WRITE_8(bufp, ELFCLASS32);	    /* elf class */
    YASM_WRITE_8(bufp, ELFDATA2LSB);	    /* data encoding :: MSB? */
    YASM_WRITE_8(bufp, EV_CURRENT);	    /* elf version */
    while (bufp-buf < EI_NIDENT)	    /* e_ident padding */
        YASM_WRITE_8(bufp, 0);

    YASM_WRITE_16_L(bufp, ET_REL);	    /* e_type - object file */
    YASM_WRITE_16_L(bufp, EM_386);	    /* e_machine - or others */
    YASM_WRITE_32_L(bufp, EV_CURRENT);	    /* elf version */
    YASM_WRITE_32_L(bufp, 0);		/* e_entry exection startaddr */
    YASM_WRITE_32_L(bufp, 0);		/* e_phoff program header off */
    YASM_WRITE_32_L(bufp, secthead_addr);   /* e_shoff section header off */
    YASM_WRITE_32_L(bufp, 0);		    /* e_flags also by arch */
    YASM_WRITE_16_L(bufp, EHDR32_SIZE);	    /* e_ehsize */
    YASM_WRITE_16_L(bufp, 0);		    /* e_phentsize */
    YASM_WRITE_16_L(bufp, 0);		    /* e_phnum */
    YASM_WRITE_16_L(bufp, SHDR32_SIZE);	    /* e_shentsize */
    YASM_WRITE_16_L(bufp, secthead_count);  /* e_shnum */
    YASM_WRITE_16_L(bufp, shstrtab_index);  /* e_shstrndx */
    *bufpp = bufp;
}

static void elf_write_proghead_x86_64(unsigned char **bufpp,
                                      elf_offset secthead_addr,
                                      unsigned long secthead_count,
                                      elf_section_index shstrtab_index)
{
    unsigned char *bufp = *bufpp;
    unsigned char *buf = bufp-4;
    YASM_WRITE_8(bufp, ELFCLASS64);	    /* elf class */
    YASM_WRITE_8(bufp, ELFDATA2LSB);	    /* data encoding :: MSB? */
    YASM_WRITE_8(bufp, EV_CURRENT);	    /* elf version */
    YASM_WRITE_8(bufp, ELFOSABI_SYSV);	    /* os/abi */
    YASM_WRITE_8(bufp, 0);		    /* SYSV v3 ABI=0 */
    while (bufp-buf < EI_NIDENT)	    /* e_ident padding */
        YASM_WRITE_8(bufp, 0);

    YASM_WRITE_16_L(bufp, ET_REL);	    /* e_type - object file */
    YASM_WRITE_16_L(bufp, EM_X86_64);	    /* e_machine - or others */
    YASM_WRITE_32_L(bufp, EV_CURRENT);	    /* elf version */
    YASM_WRITE_64Z_L(bufp, 0);		    /* e_entry */
    YASM_WRITE_64Z_L(bufp, 0);		    /* e_phoff */
    YASM_WRITE_64Z_L(bufp, secthead_addr);  /* e_shoff secthead off */

    YASM_WRITE_32_L(bufp, 0);		    /* e_flags */
    YASM_WRITE_16_L(bufp, EHDR64_SIZE);	    /* e_ehsize */
    YASM_WRITE_16_L(bufp, 0);		    /* e_phentsize */
    YASM_WRITE_16_L(bufp, 0);		    /* e_phnum */
    YASM_WRITE_16_L(bufp, SHDR64_SIZE);	    /* e_shentsize */
    YASM_WRITE_16_L(bufp, secthead_count);  /* e_shnum */
    YASM_WRITE_16_L(bufp, shstrtab_index);  /* e_shstrndx */
    *bufpp = bufp;
}

static const elf_machine_handler elf_machine_handlers[] =
{
    { "x86", "x86",
        SYMTAB32_SIZE, SYMTAB32_ALIGN, RELOC32_SIZE, SHDR32_SIZE, EHDR32_SIZE,
        elf_accept_32,
        elf_write_symtab_entry_x86,
        elf_write_secthead_x86,
        elf_write_secthead_rel_x86,
        elf_map_reloc_info_to_type_x86,
        elf_write_reloc_x86,
        elf_write_proghead_x86
    },
    { "x86", "amd64",
        SYMTAB64_SIZE, SYMTAB64_ALIGN, RELOC64_SIZE, SHDR64_SIZE, EHDR64_SIZE,
        elf_accept_8_16_32_64,
        elf_write_symtab_entry_x86_64,
        elf_write_secthead_x86_64,
        elf_write_secthead_rel_x86_64,
        elf_map_reloc_info_to_type_x86_64,
        elf_write_reloc_x86_64,
        elf_write_proghead_x86_64
    },
    { NULL }
};
static elf_machine_handler const *elf_march;

int
elf_set_arch(yasm_arch *arch)
{
    const char *machine = yasm_arch_get_machine(arch);

    /* TODO: support more than x86:x86, x86:amd64 */
    for (elf_march = elf_machine_handlers; elf_march->arch != NULL; elf_march++)
        if (yasm__strcasecmp(yasm_arch_keyword(arch), elf_march->arch)==0)
            if (yasm__strcasecmp(machine, elf_march->machine)==0)
                break;
    return elf_march->arch != NULL;
}

/* reloc functions */
/* takes ownership of addr */
elf_reloc_entry *
elf_reloc_entry_create(yasm_symrec *sym,
		       yasm_intnum *addr,
		       int rel,
		       size_t valsize)
{
    elf_reloc_entry *entry;

    if (!elf_march->accepts_reloc_size)
	yasm_internal_error(N_("Unsupported machine for ELF output"));

    if (!elf_march->accepts_reloc_size(valsize))
    {
        if (addr)
            yasm_intnum_destroy(addr);
        return NULL;
    }

    entry = yasm_xmalloc(sizeof(elf_reloc_entry));
    if (sym == NULL)
	yasm_internal_error("sym is null");

    entry->reloc.sym = sym;
    entry->reloc.addr = addr;
    entry->rtype_rel = rel;
    entry->valsize = valsize;

    return entry;
}

/* strtab functions */
elf_strtab_entry *
elf_strtab_entry_create(const char *str)
{
    elf_strtab_entry *entry = yasm_xmalloc(sizeof(elf_strtab_entry));
    entry->str = yasm__xstrdup(str);
    entry->index = 0;
    return entry;
}

elf_strtab_head *
elf_strtab_create()
{
    elf_strtab_head *strtab = yasm_xmalloc(sizeof(elf_strtab_head));
    elf_strtab_entry *entry = yasm_xmalloc(sizeof(elf_strtab_entry));

    STAILQ_INIT(strtab);
    entry->index = 0;
    entry->str = yasm__xstrdup("");

    STAILQ_INSERT_TAIL(strtab, entry, qlink);
    return strtab;
}

elf_strtab_entry *
elf_strtab_append_str(elf_strtab_head *strtab, const char *str)
{
    elf_strtab_entry *last, *entry;

    if (strtab == NULL)
	yasm_internal_error("strtab is null");
    if (STAILQ_EMPTY(strtab))
	yasm_internal_error("strtab is missing initial dummy entry");

    last = STAILQ_LAST(strtab, elf_strtab_entry, qlink);

    entry = elf_strtab_entry_create(str);
    entry->index = last->index + strlen(last->str) + 1;

    STAILQ_INSERT_TAIL(strtab, entry, qlink);
    return entry;
}

void
elf_strtab_destroy(elf_strtab_head *strtab)
{
    elf_strtab_entry *s1, *s2;

    if (strtab == NULL)
	yasm_internal_error("strtab is null");
    if (STAILQ_EMPTY(strtab))
	yasm_internal_error("strtab is missing initial dummy entry");

    s1 = STAILQ_FIRST(strtab);
    while (s1 != NULL) {
	s2 = STAILQ_NEXT(s1, qlink);
	yasm_xfree(s1->str);
	yasm_xfree(s1);
	s1 = s2;
    }
    yasm_xfree(strtab);
}

unsigned long
elf_strtab_output_to_file(FILE *f, elf_strtab_head *strtab)
{
    unsigned long size = 0;
    elf_strtab_entry *entry;

    if (strtab == NULL)
	yasm_internal_error("strtab is null");

    /* consider optimizing tables here */
    STAILQ_FOREACH(entry, strtab, qlink) {
	size_t len = 1 + strlen(entry->str);
	fwrite(entry->str, len, 1, f);
	size += len;
    }
    return size;
}



/* symtab functions */
elf_symtab_entry *
elf_symtab_entry_create(elf_strtab_entry *name,
			yasm_symrec *sym)
{
    elf_symtab_entry *entry = yasm_xmalloc(sizeof(elf_symtab_entry));
    entry->sym = sym;
    entry->sect = NULL;
    entry->name = name;
    entry->value = 0;

    entry->xsize = NULL;
    entry->size = 0;
    entry->index = 0;
    entry->bind = 0;
    entry->type = STT_NOTYPE;

    return entry;
}

static void
elf_symtab_entry_destroy(elf_symtab_entry *entry)
{
    if (entry == NULL)
	yasm_internal_error("symtab entry is null");

    if (entry->xsize)
	yasm_expr_destroy(entry->xsize);
    yasm_xfree(entry);
}

static void
elf_symrec_data_destroy(void *data)
{
    /* do nothing, as this stuff is in the symtab anyway...  this speaks of bad
     * design/use or this stuff, i fear */

    /* watch for double-free here ... */
    /*elf_symtab_entry_destroy((elf_symtab_entry *)data);*/
}

static void
elf_symtab_entry_print(void *data, FILE *f, int indent_level)
{
    elf_symtab_entry *entry = data;
    if (entry == NULL)
	yasm_internal_error("symtab entry is null");

    fprintf(f, "%*sbind=", indent_level, "");
    switch (entry->bind) {
	case STB_LOCAL:		fprintf(f, "local\n");	break;
	case STB_GLOBAL:	fprintf(f, "global\n"); break;
	case STB_WEAK:		fprintf(f, "weak\n");	break;
	default:		fprintf(f, "undef\n");	break;
    }
    fprintf(f, "%*stype=", indent_level, "");
    switch (entry->type) {
	case STT_NOTYPE:	fprintf(f, "notype\n");	break;
	case STT_OBJECT:	fprintf(f, "object\n"); break;
	case STT_FUNC:		fprintf(f, "func\n");	break;
	case STT_SECTION:	fprintf(f, "section\n");break;
	case STT_FILE:		fprintf(f, "file\n");	break;
	default:		fprintf(f, "undef\n");	break;
    }
    fprintf(f, "%*ssize=", indent_level, "");
    if (entry->xsize)
	yasm_expr_print(entry->xsize, f);
    else
	fprintf(f, "%ld", entry->size);
    fprintf(f, "\n");
}

elf_symtab_head *
elf_symtab_create()
{
    elf_symtab_head *symtab = yasm_xmalloc(sizeof(elf_symtab_head));
    elf_symtab_entry *entry = yasm_xmalloc(sizeof(elf_symtab_entry));

    STAILQ_INIT(symtab);
    entry->sym = NULL;
    entry->sect = NULL;
    entry->name = NULL;
    entry->value = 0;
    entry->xsize = NULL;
    entry->size = 0;
    entry->index = SHN_UNDEF;
    entry->bind = 0;
    entry->type = 0;
    entry->symindex = 0;
    STAILQ_INSERT_TAIL(symtab, entry, qlink);
    return symtab;
}

elf_symtab_entry *
elf_symtab_append_entry(elf_symtab_head *symtab, elf_symtab_entry *entry)
{
    if (symtab == NULL)
	yasm_internal_error("symtab is null");
    if (entry == NULL)
	yasm_internal_error("symtab entry is null");
    if (STAILQ_EMPTY(symtab))
	yasm_internal_error(N_("symtab is missing initial dummy entry"));

    STAILQ_INSERT_TAIL(symtab, entry, qlink);
    return entry;
}

elf_symtab_entry *
elf_symtab_insert_local_sym(elf_symtab_head *symtab,
			    elf_strtab_head *strtab,
			    yasm_symrec *sym)
{
    elf_strtab_entry *name = strtab
	? elf_strtab_append_str(strtab, yasm_symrec_get_name(sym))
	: NULL;
    elf_symtab_entry *entry = elf_symtab_entry_create(name, sym);
    elf_symtab_entry *after = STAILQ_FIRST(symtab);
    elf_symtab_entry *before = NULL;

    while (after && (after->bind == STB_LOCAL)) {
	before = after;
	if (before->type == STT_FILE) break;
	after = STAILQ_NEXT(after, qlink);
    }
    STAILQ_INSERT_AFTER(symtab, before, entry, qlink);

    return entry;
}

void
elf_symtab_destroy(elf_symtab_head *symtab)
{
    elf_symtab_entry *s1, *s2;

    if (symtab == NULL)
	yasm_internal_error("symtab is null");
    if (STAILQ_EMPTY(symtab))
	yasm_internal_error(N_("symtab is missing initial dummy entry"));

    s1 = STAILQ_FIRST(symtab);
    while (s1 != NULL) {
	s2 = STAILQ_NEXT(s1, qlink);
	elf_symtab_entry_destroy(s1);
	s1 = s2;
    }
    yasm_xfree(symtab);
}

unsigned long
elf_symtab_assign_indices(elf_symtab_head *symtab)
{
    elf_symtab_entry *entry, *prev=NULL;
    unsigned long last_local=0;

    if (symtab == NULL)
	yasm_internal_error("symtab is null");
    if (STAILQ_EMPTY(symtab))
	yasm_internal_error(N_("symtab is missing initial dummy entry"));

    STAILQ_FOREACH(entry, symtab, qlink) {
	if (prev)
	    entry->symindex = prev->symindex + 1;
	if (entry->bind == STB_LOCAL)
	    last_local = entry->symindex;
	prev = entry;
    }
    return last_local + 1;
}

unsigned long
elf_symtab_write_to_file(FILE *f, elf_symtab_head *symtab)
{
    unsigned char buf[SYMTAB_MAXSIZE], *bufp;
    elf_symtab_entry *entry, *prev;
    unsigned long size = 0;

    if (!symtab)
	yasm_internal_error(N_("symtab is null"));

    prev = NULL;
    STAILQ_FOREACH(entry, symtab, qlink) {

	yasm_intnum *size_intn=NULL, *value_intn=NULL;
	bufp = buf;

	/* get size (if specified); expr overrides stored integer */
	if (entry->xsize) {
	    size_intn = yasm_intnum_copy(
		yasm_expr_get_intnum(&entry->xsize, yasm_common_calc_bc_dist));
	    if (!size_intn)
		yasm__error(entry->xsize->line,
		    N_("size specifier not an integer expression"));
	}
	else
	    size_intn = yasm_intnum_create_uint(entry->size);

	/* get EQU value for constants */
	if (entry->sym) {
	    const yasm_expr *equ_expr_c;
	    equ_expr_c = yasm_symrec_get_equ(entry->sym);

	    if (equ_expr_c != NULL) {
		const yasm_intnum *equ_intn;
		yasm_expr *equ_expr = yasm_expr_copy(equ_expr_c);
		equ_intn = yasm_expr_get_intnum(&equ_expr,
						yasm_common_calc_bc_dist);

		if (equ_intn == NULL) {
		    yasm__error(equ_expr->line,
				N_("EQU value not an integer expression"));
		}

		value_intn = yasm_intnum_copy(equ_intn);
		entry->index = SHN_ABS;
		yasm_expr_destroy(equ_expr);
	    }
	}
	if (value_intn == NULL)
	    value_intn = yasm_intnum_create_uint(entry->value);


        if (!elf_march->write_symtab_entry || !elf_march->symtab_entry_size)
            yasm_internal_error(N_("Unsupported machine for ELF output"));
        elf_march->write_symtab_entry(bufp, entry, value_intn, size_intn);
        fwrite(buf, elf_march->symtab_entry_size, 1, f);
        size += elf_march->symtab_entry_size;

	yasm_intnum_destroy(size_intn);
	yasm_intnum_destroy(value_intn);

	prev = entry;
    }
    return size;
}

void elf_symtab_set_nonzero(elf_symtab_entry *entry,
			    yasm_section *sect,
			    elf_section_index sectidx,
			    elf_symbol_binding bind,
			    elf_symbol_type type,
			    yasm_expr *xsize,
			    elf_address value)
{
    if (!entry)
	yasm_internal_error("NULL entry");
    if (sect) entry->sect = sect;
    if (sectidx) entry->index = sectidx;
    if (bind) entry->bind = bind;
    if (type) entry->type = type;
    if (xsize) entry->xsize = xsize;
    if (value) entry->value = value;
}


elf_secthead *
elf_secthead_create(elf_strtab_entry	*name,
		    elf_section_type	 type,
		    elf_section_flags	 flags,
		    elf_section_index	 idx,
		    elf_address		 offset,
		    elf_size		 size)
{
    elf_secthead *esd = yasm_xmalloc(sizeof(elf_secthead));

    esd->type = type;
    esd->flags = flags;
    esd->offset = offset;
    esd->size = yasm_intnum_create_uint(size);
    esd->link = 0;
    esd->info = 0;
    esd->align = NULL;
    esd->entsize = 0;
    esd->index = idx;

    esd->sym = NULL;
    esd->name = name;
    esd->index = 0;
    esd->rel_name = NULL;
    esd->rel_index = idx;
    esd->rel_offset = 0;
    esd->nreloc = 0;

    if (name && (strcmp(name->str, ".symtab") == 0)) {
        if (!elf_march->symtab_entry_size || !elf_march->symtab_entry_align)
	    yasm_internal_error(N_("unsupported ELF format"));
        esd->entsize = elf_march->symtab_entry_size;
        esd->align = yasm_intnum_create_uint(elf_march->symtab_entry_align);
    }

    return esd;
}

void
elf_secthead_destroy(elf_secthead *shead)
{
    if (shead == NULL)
	yasm_internal_error(N_("shead is null"));

    if (shead->align)
	yasm_intnum_destroy(shead->align);

    yasm_xfree(shead);
}

static void
elf_section_data_destroy(void *data)
{
    elf_secthead_destroy((elf_secthead *)data);
}

static void
elf_secthead_print(void *data, FILE *f, int indent_level)
{
    elf_secthead *sect = data;
    fprintf(f, "%*sname=%s\n", indent_level, "",
	    sect->name ? sect->name->str : "<undef>");
    fprintf(f, "%*ssym=\n", indent_level, "");
    yasm_symrec_print(sect->sym, f, indent_level+1);
    fprintf(f, "%*sindex=0x%x\n", indent_level, "", sect->index);
    fprintf(f, "%*sflags=", indent_level, "");
    if (sect->flags & SHF_WRITE)
	fprintf(f, "WRITE ");
    if (sect->flags & SHF_ALLOC)
	fprintf(f, "ALLOC ");
    if (sect->flags & SHF_EXECINSTR)
	fprintf(f, "EXEC ");
    /*if (sect->flags & SHF_MASKPROC)
	fprintf(f, "PROC-SPECIFIC"); */
    fprintf(f, "%*soffset=0x%lx\n", indent_level, "", sect->offset);
    fprintf(f, "%*ssize=0x%lx\n", indent_level, "",
	    yasm_intnum_get_uint(sect->size));
    fprintf(f, "%*slink=0x%x\n", indent_level, "", sect->link);
    fprintf(f, "%*salign=%ld\n", indent_level, "",
	    yasm_intnum_get_uint(sect->align));
    fprintf(f, "%*snreloc=%ld\n", indent_level, "", sect->nreloc);
}

unsigned long
elf_secthead_write_to_file(FILE *f, elf_secthead *shead,
			   elf_section_index sindex)
{
    unsigned char buf[SHDR_MAXSIZE], *bufp = buf;
    shead->index = sindex;

    if (shead == NULL)
	yasm_internal_error("shead is null");

    if (!elf_march->write_secthead || !elf_march->secthead_size)
        yasm_internal_error(N_("Unsupported machine for ELF output"));
    elf_march->write_secthead(bufp, shead);
    if (fwrite(buf, elf_march->secthead_size, 1, f))
        return elf_march->secthead_size;
    yasm_internal_error(N_("Failed to write an elf section header"));
    return 0;
}

int
elf_secthead_append_reloc(yasm_section *sect, elf_secthead *shead,
			  elf_reloc_entry *reloc)
{
    int new_sect = 0;

    if (sect == NULL)
	yasm_internal_error("sect is null");
    if (shead == NULL)
	yasm_internal_error("shead is null");
    if (reloc == NULL)
	yasm_internal_error("reloc is null");

    shead->nreloc++;
    yasm_section_add_reloc(sect, (yasm_reloc *)reloc, yasm_xfree);

    return new_sect;
}

unsigned long
elf_secthead_write_rel_to_file(FILE *f, elf_section_index symtab_idx,
			       yasm_section *sect, elf_secthead *shead,
			       elf_section_index sindex)
{
    unsigned char buf[SHDR_MAXSIZE], *bufp = buf;

    if (shead == NULL)
	yasm_internal_error("shead is null");

    if (!yasm_section_relocs_first(sect))
	return 0;  	/* no relocations, no .rel.* section header */

    shead->rel_index = sindex;

    if (!elf_march->write_secthead_rel || !elf_march->secthead_size)
        yasm_internal_error(N_("Unsupported machine for ELF output"));
    elf_march->write_secthead_rel(bufp, shead, symtab_idx, sindex);
    if (fwrite(buf, elf_march->secthead_size, 1, f))
        return elf_march->secthead_size;
    yasm_internal_error(N_("Failed to write an elf section header"));
    return 0;
}

unsigned long
elf_secthead_write_relocs_to_file(FILE *f, yasm_section *sect,
				  elf_secthead *shead)
{
    elf_reloc_entry *reloc;
    unsigned char buf[RELOC_MAXSIZE], *bufp;
    unsigned long size = 0;
    long pos;

    if (shead == NULL)
	yasm_internal_error("shead is null");

    reloc = (elf_reloc_entry *)yasm_section_relocs_first(sect);
    if (!reloc)
	return 0;

    /* first align section to multiple of 4 */
    pos = ftell(f);
    if (pos == -1)
	yasm__error(0, N_("couldn't read position on output stream"));
    pos = (pos + 3) & ~3;
    if (fseek(f, pos, SEEK_SET) < 0)
	yasm__error(0, N_("couldn't seek on output stream"));
    shead->rel_offset = (unsigned long)pos;


    while (reloc) {
	yasm_sym_vis vis;
	unsigned char r_type=0, r_sym;
	elf_symtab_entry *esym;

	esym = yasm_symrec_get_data(reloc->reloc.sym, &elf_symrec_data);
	if (esym)
	    r_sym = esym->symindex;
	else
	    r_sym = STN_UNDEF;

	vis = yasm_symrec_get_visibility(reloc->reloc.sym);
        if (!elf_march->map_reloc_info_to_type)
            yasm_internal_error(N_("Unsupported arch/machine for elf output"));
        r_type = elf_march->map_reloc_info_to_type(reloc);

	bufp = buf;
        if (!elf_march->write_reloc || !elf_march->reloc_entry_size)
            yasm_internal_error(N_("Unsupported arch/machine for elf output"));
        elf_march->write_reloc(bufp, reloc, r_type, r_sym);
        fwrite(buf, elf_march->reloc_entry_size, 1, f);
        size += elf_march->reloc_entry_size;

	reloc = (elf_reloc_entry *)
	    yasm_section_reloc_next((yasm_reloc *)reloc);
    }
    return size;
}

elf_section_type
elf_secthead_get_type(elf_secthead *shead)
{
    return shead->type;
}

int
elf_secthead_is_empty(elf_secthead *shead)
{
    return yasm_intnum_is_zero(shead->size);
}

yasm_symrec *
elf_secthead_get_sym(elf_secthead *shead)
{
    return shead->sym;
}

elf_section_index
elf_secthead_get_index(elf_secthead *shead)
{
    return shead->index;
}

const yasm_intnum *
elf_secthead_set_align(elf_secthead *shead, yasm_intnum *align)
{
    if (shead->align != NULL)
	yasm_intnum_destroy(shead->align);
    
    return shead->align = align;
}

elf_section_info
elf_secthead_set_info(elf_secthead *shead, elf_section_info info)
{
    return shead->info = info;
}

elf_section_index
elf_secthead_set_index(elf_secthead *shead, elf_section_index sectidx)
{
    return shead->index = sectidx;
}

elf_section_index
elf_secthead_set_link(elf_secthead *shead, elf_section_index link)
{
    return shead->link = link;
}

elf_section_index
elf_secthead_set_rel_index(elf_secthead *shead, elf_section_index sectidx)
{
    return shead->rel_index = sectidx;
}

elf_strtab_entry *
elf_secthead_set_rel_name(elf_secthead *shead, elf_strtab_entry *entry)
{
    return shead->rel_name = entry;
}

elf_size
elf_secthead_set_entsize(elf_secthead *shead, elf_size size)
{
    return shead->entsize = size;
}

yasm_symrec *
elf_secthead_set_sym(elf_secthead *shead, yasm_symrec *sym)
{
    return shead->sym = sym;
}

void
elf_secthead_add_size(elf_secthead *shead, yasm_intnum *size)
{
    if (size) {
	yasm_intnum_calc(shead->size, YASM_EXPR_ADD, size, 0);
    }
}

long
elf_secthead_set_file_offset(elf_secthead *shead, long pos)
{
    unsigned long align = yasm_intnum_get_uint(shead->align);

    if (align == 0 || align == 1) {
	shead->offset = (unsigned long)pos;
	return pos;
    }
    else if (align & (align - 1))
	yasm_internal_error(
	    N_("alignment %d for section `%s' is not a power of 2"));
	    /*, align, sect->name->str);*/

    shead->offset = (unsigned long)((pos + align - 1) & ~(align - 1));
    return (long)shead->offset;
}

unsigned long
elf_proghead_get_size(void)
{
    if (!elf_march->proghead_size)
	yasm_internal_error(N_("Unsupported ELF format for output"));
    return elf_march->proghead_size;
}

unsigned long
elf_proghead_write_to_file(FILE *f,
			   elf_offset secthead_addr,
			   unsigned long secthead_count,
			   elf_section_index shstrtab_index)
{
    unsigned char buf[EHDR_MAXSIZE], *bufp = buf;

    YASM_WRITE_8(bufp, ELFMAG0);		/* ELF magic number */
    YASM_WRITE_8(bufp, ELFMAG1);
    YASM_WRITE_8(bufp, ELFMAG2);
    YASM_WRITE_8(bufp, ELFMAG3);

    if (!elf_march->write_proghead || !elf_march->proghead_size)
	yasm_internal_error(N_("Unsupported ELF format for output"));
    elf_march->write_proghead(&bufp, secthead_addr, secthead_count, shstrtab_index);

    if (((unsigned)(bufp - buf)) != elf_march->proghead_size)
        yasm_internal_error(N_("ELF program header is not proper length"));

    if (fwrite(buf, elf_march->proghead_size, 1, f))
        return elf_march->proghead_size;

    yasm_internal_error(N_("Failed to write ELF program header"));
    return 0;
}
