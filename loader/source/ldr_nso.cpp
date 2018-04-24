#include <switch.h>
#include <algorithm>
#include <cstdio>
#include "ldr_nso.hpp"

static NsoUtils::NsoHeader g_nso_headers[NSO_NUM_MAX] = {0};
static bool g_nso_present[NSO_NUM_MAX] = {0};

static char g_nso_path[FS_MAX_PATH] = {0};

FILE *NsoUtils::OpenNsoFromExeFS(unsigned int index) {
    std::fill(g_nso_path, g_nso_path + FS_MAX_PATH, 0);
    snprintf(g_nso_path, FS_MAX_PATH, "code:/%s", NsoUtils::GetNsoFileName(index));
    return fopen(g_nso_path, "rb");
}

FILE *NsoUtils::OpenNsoFromSdCard(unsigned int index, u64 title_id) {  
    std::fill(g_nso_path, g_nso_path + FS_MAX_PATH, 0);
    snprintf(g_nso_path, FS_MAX_PATH, "sdmc:/atmosphere/titles/%016lx/exefs/%s", title_id, NsoUtils::GetNsoFileName(index));
    return fopen(g_nso_path, "rb");
}

FILE *NsoUtils::OpenNso(unsigned int index, u64 title_id) {
    FILE *f_out = OpenNsoFromSdCard(index, title_id);
    if (f_out != NULL) {
        return f_out;
    }
    return OpenNsoFromExeFS(index);
}

bool NsoUtils::IsNsoPresent(unsigned int index) {
    return g_nso_present[index];
}

Result NsoUtils::LoadNsoHeaders(u64 title_id) {
    FILE *f_nso;
    
    /* Zero out the cache. */
    std::fill(g_nso_present, g_nso_present + NSO_NUM_MAX, false);
    std::fill(g_nso_headers, g_nso_headers + NSO_NUM_MAX, (const NsoUtils::NsoHeader &){0});
    
    for (unsigned int i = 0; i < NSO_NUM_MAX; i++) {
        f_nso = OpenNso(i, title_id);
        if (f_nso != NULL) {
            if (fread(&g_nso_headers[i], sizeof(NsoUtils::NsoHeader), 1, f_nso) != sizeof(NsoUtils::NsoHeader)) {
                return 0xA09;
            }
            g_nso_present[i] = true;
            fclose(f_nso);
            continue;
        }
        if (1 < i && i < 12) {
            /* If we failed to open a subsdk, there are no more subsdks. */
            i = 11;
        }
    }
    
    return 0x0;
}

Result NsoUtils::ValidateNsoLoadSet() {
    /* We *must* have a "main" NSO. */
    if (!g_nso_present[1]) {
        return 0xA09;
    }
    
    /* Behavior switches depending on whether we have an rtld. */
    if (g_nso_present[0]) {
         /* If we have an rtld, dst offset for .text must be 0 for all other NSOs. */
        for (unsigned int i = 0; i < NSO_NUM_MAX; i++) {
            if (g_nso_present[i] && g_nso_headers[i].segments[0].dst_offset != 0) {
                return 0xA09;
            }
        }
    } else {
        /* If we don't have an rtld, we must ONLY have a main. */
        for (unsigned int i = 2; i < NSO_NUM_MAX; i++) {
            if (g_nso_present[i]) {
                return 0xA09;
            }
        }
        /* That main's .text must be at dst_offset 0. */
        if (g_nso_headers[1].segments[0].dst_offset != 0) {
            return 0xA09;
        }
    }
    
    return 0x0;
}


Result NsoUtils::CalculateNsoLoadExtents(u32 addspace_type, u32 args_size, NsoLoadExtents *extents) {
    *extents = (const NsoUtils::NsoLoadExtents){0};
    /* Calculate base offsets. */
    for (unsigned int i = 0; i < NSO_NUM_MAX; i++) {
        if (g_nso_present[i]) {
            extents->nso_addresses[i] = extents->total_size;
            u32 text_end = g_nso_headers[i].segments[0].dst_offset + g_nso_headers[i].segments[0].decomp_size;
            u32 ro_end = g_nso_headers[i].segments[1].dst_offset + g_nso_headers[i].segments[1].decomp_size;
            u32 rw_end = g_nso_headers[i].segments[2].dst_offset + g_nso_headers[i].segments[2].decomp_size + g_nso_headers[i].segments[2].align_or_total_size;
            extents->nso_sizes[i] = text_end;
            if (extents->nso_sizes[i] < ro_end) {
                extents->nso_sizes[i] = ro_end;
            }
            if (extents->nso_sizes[i] < rw_end) {
                extents->nso_sizes[i] = rw_end;
            }
            extents->nso_sizes[i] += 0xFFF;
            extents->nso_sizes[i] &= ~0xFFFULL;
            extents->total_size += extents->nso_sizes[i];
            if (args_size && !extents->args_size) {
                extents->args_address = extents->total_size;
                /* What the fuck? Where does 0x9007 come from? */
                extents->args_size = (2 * args_size + 0x9007);
                extents->args_size &= ~0xFFFULL;
            }
        }
    }
    
    /* Calculate ASLR extents for address space type. */
    u64 addspace_start, addspace_size, addspace_end;
    if (kernelAbove200()) {
        switch (addspace_type & 0xE) {
            case 0:
            case 4:
                addspace_start = 0x200000ULL;
                addspace_size = 0x3FE00000ULL;
                break;
            case 2:
                addspace_start = 0x8000000ULL;
                addspace_size = 0x78000000ULL;
                break;
            case 6:
                addspace_start = 0x8000000ULL;
                addspace_size = 0x7FF8000000ULL;
                break;
            default:
                /* TODO: Panic. */
                return 0xD001;
        }
    } else {
        if (addspace_type & 2) {
            addspace_start = 0x8000000ULL;
            addspace_size = 0x78000000ULL;
        } else {
            addspace_start = 0x200000ULL;
            addspace_size = 0x3FE00000ULL;
        }
    }
    addspace_end = addspace_start + addspace_size;
    if (addspace_start + extents->total_size > addspace_end) {
        return 0xD001;
    }
    
    u64 aslr_slide = 0;
    if (addspace_type & 0x20) {
        /* TODO: Apply a random ASLR slide. */
    }
    
    extents->base_address = addspace_start + aslr_slide;
    for (unsigned int i = 0; i < NSO_NUM_MAX; i++) {
        if (g_nso_present[i]) {
            extents->nso_addresses[i] += extents->base_address;
        }
    }
    if (extents->args_address) {
        extents->args_address += extents->base_address;
    }
    
    return 0x0;
}