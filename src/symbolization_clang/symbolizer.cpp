// SPDX-License-Identifier: LGPL-2.1-or-later

#include "symbolizer.h"

#include <libdwfl.h>
#include <cxxabi.h>
#include <dwarf.h>

#include <tuple>
#include <algorithm>
#include <filesystem>

namespace {
const Dwfl_Callbacks s_callbacks = {
    &dwfl_build_id_find_elf,
    &dwfl_standard_find_debuginfo,
    &dwfl_offline_section_address,
    nullptr
};

auto cleanPath(const std::string_view &path)
{
    return std::filesystem::path(path).lexically_normal().string();
}

const char *dieName(Dwarf_Die *die)
{
    Dwarf_Attribute attr;
    Dwarf_Attribute *result = dwarf_attr_integrate(die, DW_AT_MIPS_linkage_name, &attr);
    if (!result)
        result = dwarf_attr_integrate(die, DW_AT_linkage_name, &attr);

    auto *name = dwarf_formstring(result);
    if (name)
        return name;

    return dwarf_diename(die);
}

Symbol inlinedSubroutineSymbol(Dwarf_Die *scope, Dwarf_Files *files, Symbol &nonInlined)
{
    Dwarf_Attribute attr;
    Dwarf_Word val = 0;

    Symbol symbol;
    symbol.isInlined = true;
    symbol.name = dieName(scope);

    const char *file = nullptr;
    if (dwarf_formudata(dwarf_attr(scope, DW_AT_call_file, &attr), &val) == 0)
        file = dwarf_filesrc(files, val, nullptr, nullptr);
    symbol.file = file ? cleanPath(file) : "??";

    if (dwarf_formudata(dwarf_attr(scope, DW_AT_call_line, &attr), &val) == 0)
        symbol.line = static_cast<int>(val);

    if (dwarf_formudata(dwarf_attr(scope, DW_AT_call_column, &attr), &val) == 0)
        symbol.column = static_cast<int>(val);

    std::swap(nonInlined.file, symbol.file);
    std::swap(nonInlined.line, symbol.line);
    std::swap(nonInlined.column, symbol.column);

    return symbol;
}

std::vector<Symbol> inlinedSymbols(Symbol nonInlined, Dwarf_Die *cuDie, uint64_t offset)
{
    // innermost scope DIE
    Dwarf_Die scopeDie;
    {
        Dwarf_Die *scopes = nullptr;
        const auto nscopes = dwarf_getscopes(cuDie, offset, &scopes);
        if (nscopes == 0)
            return {std::move(nonInlined)};
        scopeDie = scopes[0];
        free(scopes);
    }

    Dwarf_Die *scopes = nullptr;
    const auto nscopes = dwarf_getscopes_die(&scopeDie, &scopes);

    Dwarf_Files *files = nullptr;
    dwarf_getsrcfiles(cuDie, &files, nullptr);

    std::vector<Symbol> symbols;
    for (int i = 0; i < nscopes; ++i) {
        const auto scope = &scopes[i];
        if (dwarf_tag(scope) == DW_TAG_inlined_subroutine)
            symbols.push_back(inlinedSubroutineSymbol(scope, files, nonInlined));
    }
    free(scopes);
    symbols.push_back(std::move(nonInlined));
    return symbols;
}

struct AddrRange
{
    Dwarf_Addr low = 0;
    Dwarf_Addr high = 0;

    void setMinMax(const AddrRange range)
    {
        if (range.low && (low == 0 || low > range.low))
            low = range.low;
        if (range.high && (high == 0 || high < range.high))
            high = range.high;
    }

    bool contains(Dwarf_Addr addr) const
    {
        return low <= addr && addr < high;
    }

    bool operator<(const AddrRange &rhs) const
    {
        return std::tie(low, high) < std::tie(rhs.low, high);
    }
};

struct DieRangeMap
{
    DieRangeMap(Dwarf_Die *die = nullptr, Dwarf_Addr bias = 0)
        : die(die)
        , bias(bias)
    {
        if (die)
            gatherRanges(die, bias);
    }

    bool contains(Dwarf_Addr addr) const
    {
        if (!range.contains(addr))
            return false;
        return std::any_of(ranges.begin(), ranges.end(),
                           [addr](AddrRange range) {
                                return range.contains(addr);
                           });
    }

    bool operator<(const DieRangeMap &rhs) const
    {
        return range < rhs.range;
    }

    Dwarf_Die *die = nullptr;
    AddrRange range; // may be non-continuous, but allows quick checks and sorting
    std::vector<AddrRange> ranges;
    Dwarf_Addr bias;

private:
    void gatherRanges(Dwarf_Die *parent_die, Dwarf_Addr bias)
    {
        Dwarf_Die die;
        if (dwarf_child(parent_die, &die) != 0)
            return;

        do {
            switch (dwarf_tag(&die)) {
            case DW_TAG_subprogram:
            case DW_TAG_inlined_subroutine:
                addRanges(&die, bias);
                break;
            };
            bool declaration = false;
            Dwarf_Attribute attr_mem;
            dwarf_formflag(dwarf_attr(&die, DW_AT_declaration, &attr_mem), &declaration);
            if (!declaration) {
                // let's be curious and look deeper in the tree,
                // function are not necessarily at the first level, but
                // might be nested inside a namespace, structure etc.
                gatherRanges(&die, bias);
            }
        } while (dwarf_siblingof(&die, &die) == 0);
    }

    void addRanges(Dwarf_Die *die, Dwarf_Addr bias)
    {
        Dwarf_Addr low = 0, high = 0;
        Dwarf_Addr base = 0;
        ptrdiff_t offset = 0;
        while ((offset = dwarf_ranges(die, offset, &base, &low, &high)) > 0) {
            addRange(low, high, bias);
        }
    }

    void addRange(Dwarf_Addr low, Dwarf_Addr high, Dwarf_Addr bias)
    {
        AddrRange ret;
        ret.low = low + bias;
        ret.high = high + bias;
        range.setMinMax(ret);
        ranges.push_back(ret);
    }
};

class DieRangeMaps
{
public:
    DieRangeMaps(Dwfl_Module *mod = nullptr)
    {
        if (!mod)
            return;

        Dwarf_Die *die = nullptr;
        Dwarf_Addr bias = 0;
        while ((die = dwfl_module_nextcu(mod, die, &bias))) {
            DieRangeMap map(die, bias);
            if (map.range.low == 0 && map.range.high == 0) {
                // no range entries, skip
                continue;
            }
            range.setMinMax(map.range);
            maps.push_back(std::move(map));
        }
    }

    Dwarf_Die *findDie(Dwarf_Addr addr, Dwarf_Addr *bias) const
    {
        if (!range.contains(addr))
            return nullptr;

        auto it = std::find_if(maps.begin(), maps.end(),
                               [addr](const DieRangeMap &map) {
                                    return map.contains(addr);
                               });
        if (it == maps.end())
            return nullptr;

        *bias = it->bias;
        return it->die;
    }
public:
    AddrRange range; // may be non-continuous, but allows quick checks
    std::vector<DieRangeMap> maps;
};
}

class Symbolizer::ModuleDieRangeMaps
{
public:
    auto moduleAddrDie(Dwfl_Module *mod, Dwarf_Addr addr, Dwarf_Addr *bias)
    {
        auto it = dieRangeMaps.find(mod);
        if (it == dieRangeMaps.end())
            it = dieRangeMaps.insert({mod, DieRangeMaps(mod)}).first;
        return it->second.findDie(addr, bias);
    }
private:
    std::unordered_map<Dwfl_Module *, DieRangeMaps> dieRangeMaps;
};

namespace {
auto moduleAddrDie(Dwfl_Module *mod, Dwarf_Addr addr, Dwarf_Addr *bias, std::unique_ptr<Symbolizer::ModuleDieRangeMaps> *moduleRangeMaps)
{
    auto die = dwfl_module_addrdie(mod, addr, bias);
    if (die)
        return die;

    // broken DWARF emitter by clang, e.g. no aranges
    // cf.: https://sourceware.org/ml/elfutils-devel/2017-q2/msg00180.html
    // build a custom lookup table and query that one
    if (!*moduleRangeMaps)
        *moduleRangeMaps = std::make_unique<Symbolizer::ModuleDieRangeMaps>();

    return (*moduleRangeMaps)->moduleAddrDie(mod, addr, bias);
}
}

Symbolizer::Symbolizer()
    : m_dwfl(dwfl_begin(&s_callbacks))
{
}

Symbolizer::~Symbolizer()
{
    dwfl_end(m_dwfl);
}

void Symbolizer::beginReportElf()
{
    dwfl_report_begin(m_dwfl);
}

void Symbolizer::reportElf(const std::string &path, uint64_t addr)
{
    if (!dwfl_report_elf(m_dwfl, path.c_str(), path.c_str(), -1, addr, false))
        fprintf(stderr, "failed to report elf %s at %zx: %s\n", path.c_str(), addr, dwfl_errmsg(dwfl_errno()));
}

void Symbolizer::endReportElf()
{
    if (dwfl_report_end(m_dwfl, nullptr, nullptr) != 0)
        fprintf(stderr, "failed to end elf reporting: %s\n", dwfl_errmsg(dwfl_errno()));
}

IpInfo Symbolizer::ipInfo(uint64_t ip)
{
    auto *mod = dwfl_addrmodule(m_dwfl, ip);
    if (!mod) {
        fprintf(stderr, "failed to find module for ip %zx: %s\n", ip, dwfl_errmsg(dwfl_errno()));
        return {};
    }

    IpInfo info;

    // dso info
    {
        Dwarf_Addr moduleStart = 0;
        info.dso = dwfl_module_info(mod, nullptr, &moduleStart, nullptr, nullptr, nullptr, nullptr, nullptr);
        info.dso_offset = ip - moduleStart;
    }

    // sym info
    Symbol nonInlined;
    {
        GElf_Sym sym;
        auto symname = dwfl_module_addrinfo(mod, ip, &info.symbol_offset, &sym, nullptr, nullptr, nullptr);
        if (!symname)
            symname = "??";
        nonInlined.name = symname;
    }

    Dwarf_Addr bias = 0;
    // CU DIE: Compilation Unit Debug Information Entry
    auto cuDie = moduleAddrDie(mod, ip, &bias, &m_rangeMaps);
    if (cuDie) {
        // srcfile info, potentially for the first inlined symbol, we will fix that up later on
        const auto offset = ip - bias;
        auto srcloc = dwarf_getsrc_die(cuDie, offset);
        if (srcloc) {
            auto srcfile = dwarf_linesrc(srcloc, nullptr, nullptr);
            if (srcfile) {
                nonInlined.file = cleanPath(srcfile);
                dwarf_lineno(srcloc, &nonInlined.line);
                dwarf_linecol(srcloc, &nonInlined.column);
            }
        }
        info.symbols = inlinedSymbols(std::move(nonInlined), cuDie, offset);
    } else {
        info.symbols = {std::move(nonInlined)};
    }

    return info;
}

std::string Symbolizer::demangle(const std::string &symbol) const
{
    if (symbol.size() < 3 || symbol[0] != '_' || symbol[1] != 'Z')
        return symbol;
    auto demangled = abi::__cxa_demangle(symbol.c_str(), nullptr, nullptr, nullptr);
    if (!demangled)
        return symbol;
    std::string ret = demangled;
    free(demangled);
    return ret;
}
