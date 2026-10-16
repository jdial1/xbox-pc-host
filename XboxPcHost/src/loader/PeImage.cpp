#include "pch.h"
#include "PeImage.h"
#include "Log.h"

namespace
{
    bool ReadWholeFile(std::filesystem::path const& path, std::vector<uint8_t>& data)
    {
        FILE* f = nullptr;
        if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        bool ok = size > 0;
        if (ok)
        {
            data.resize(static_cast<size_t>(size));
            ok = fread(data.data(), 1, data.size(), f) == data.size();
        }
        fclose(f);
        return ok;
    }

    ULONG SectionProtection(DWORD characteristics)
    {
        bool x = characteristics & IMAGE_SCN_MEM_EXECUTE;
        bool w = characteristics & IMAGE_SCN_MEM_WRITE;
        bool r = characteristics & IMAGE_SCN_MEM_READ;
        if (x) return w ? PAGE_EXECUTE_READWRITE : PAGE_EXECUTE_READ;
        if (w) return PAGE_READWRITE;
        return r ? PAGE_READONLY : PAGE_NOACCESS;
    }
}

PeImage::~PeImage()
{
    if (m_functionTable) RtlDeleteFunctionTable(m_functionTable);
    if (m_base) VirtualFree(m_base, 0, MEM_RELEASE);
}

IMAGE_NT_HEADERS64* PeImage::Nt() const
{
    return At<IMAGE_NT_HEADERS64>(static_cast<uint32_t>(At<IMAGE_DOS_HEADER>(0)->e_lfanew));
}

bool PeImage::Load(std::filesystem::path const& path, ImportResolver& resolver, std::wstring& error)
{
    m_path = path;
    std::vector<uint8_t> file;
    if (!ReadWholeFile(path, file)) { error = L"can't read file"; return false; }

    // Validate the headers against the file before trusting any offsets.
    if (file.size() < sizeof(IMAGE_DOS_HEADER)) { error = L"file too small"; return false; }
    auto dos = reinterpret_cast<IMAGE_DOS_HEADER*>(file.data());
    if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 ||
        static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > file.size())
    {
        error = L"not a PE file";
        return false;
    }
    auto nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { error = L"not a PE file"; return false; }
    if (nt->FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
    {
        error = L"not an x64 image";
        return false;
    }

    m_size = nt->OptionalHeader.SizeOfImage;
    m_isDll = (nt->FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
    const uint64_t preferred = nt->OptionalHeader.ImageBase;

    // Try the preferred base first so images without relocations still work.
    m_base = static_cast<uint8_t*>(VirtualAllocFromApp(reinterpret_cast<void*>(preferred), m_size,
        MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!m_base)
    {
        if (nt->FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)
        {
            error = L"preferred base is taken and the image has no relocations";
            return false;
        }
        m_base = static_cast<uint8_t*>(VirtualAllocFromApp(nullptr, m_size, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    }
    if (!m_base) { error = L"out of memory mapping image"; return false; }

    // Headers, then each section's raw data. Uninitialized tails stay zero.
    memcpy(m_base, file.data(), std::min<size_t>(nt->OptionalHeader.SizeOfHeaders, file.size()));
    auto section = IMAGE_FIRST_SECTION(nt);
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        if (section->VirtualAddress >= m_size) { error = L"section outside image"; return false; }
        size_t raw = std::min<size_t>(section->SizeOfRawData, m_size - section->VirtualAddress);
        if (section->Misc.VirtualSize) raw = std::min<size_t>(raw, section->Misc.VirtualSize);
        if (raw == 0) continue;
        if (static_cast<size_t>(section->PointerToRawData) + raw > file.size()) { error = L"section data past end of file"; return false; }
        memcpy(m_base + section->VirtualAddress, file.data() + section->PointerToRawData, raw);
    }

    intptr_t delta = reinterpret_cast<intptr_t>(m_base) - static_cast<intptr_t>(preferred);
    if (delta != 0)
    {
        if (!ApplyRelocations(delta, error)) return false;
        Nt()->OptionalHeader.ImageBase = reinterpret_cast<uint64_t>(m_base);
        m_relocated = true;
    }

    if (!BindImports(resolver, error)) return false;
    ReadTls();
    if (!ProtectSections(error)) return false;
    RegisterExceptionTable();
    FlushInstructionCache(GetCurrentProcess(), m_base, m_size);

    if (Nt()->OptionalHeader.AddressOfEntryPoint)
        m_entry = m_base + Nt()->OptionalHeader.AddressOfEntryPoint;

    HOST_LOG(L"Mapped %s at %p (%zu KB%s)", path.filename().c_str(), m_base, m_size / 1024, m_relocated ? L", relocated" : L"");
    return true;
}

bool PeImage::ApplyRelocations(intptr_t delta, std::wstring& error)
{
    auto const& dir = Nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_BASERELOC];
    if (!dir.VirtualAddress || !dir.Size) return true; // nothing to fix up

    uint32_t offset = 0;
    while (offset + sizeof(IMAGE_BASE_RELOCATION) <= dir.Size)
    {
        auto block = At<IMAGE_BASE_RELOCATION>(dir.VirtualAddress + offset);
        if (block->SizeOfBlock < sizeof(IMAGE_BASE_RELOCATION)) break;
        size_t count = (block->SizeOfBlock - sizeof(IMAGE_BASE_RELOCATION)) / sizeof(WORD);
        auto entries = reinterpret_cast<WORD*>(block + 1);
        for (size_t i = 0; i < count; ++i)
        {
            uint32_t rva = block->VirtualAddress + (entries[i] & 0xFFF);
            switch (entries[i] >> 12)
            {
            case IMAGE_REL_BASED_ABSOLUTE: break;
            case IMAGE_REL_BASED_DIR64: *At<uint64_t>(rva) += static_cast<uint64_t>(delta); break;
            case IMAGE_REL_BASED_HIGHLOW: *At<uint32_t>(rva) += static_cast<uint32_t>(delta); break;
            default:
                error = L"unsupported relocation type " + std::to_wstring(entries[i] >> 12);
                return false;
            }
        }
        offset += block->SizeOfBlock;
    }
    return true;
}

bool PeImage::BindImports(ImportResolver& resolver, std::wstring& error)
{
    auto const& dir = Nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir.VirtualAddress && dir.Size)
    {
        for (auto desc = At<IMAGE_IMPORT_DESCRIPTOR>(dir.VirtualAddress); desc->Name; ++desc)
        {
            const char* dll = At<const char>(desc->Name);
            auto lookup = At<uint64_t>(desc->OriginalFirstThunk ? desc->OriginalFirstThunk : desc->FirstThunk);
            auto iat = At<uint64_t>(desc->FirstThunk);
            for (; *lookup; ++lookup, ++iat)
            {
                void* target;
                if (IMAGE_SNAP_BY_ORDINAL64(*lookup))
                    target = resolver.ResolveImport(dll, nullptr, static_cast<uint16_t>(IMAGE_ORDINAL64(*lookup)));
                else
                    target = resolver.ResolveImport(dll, At<IMAGE_IMPORT_BY_NAME>(static_cast<uint32_t>(*lookup))->Name, 0);
                if (!target)
                {
                    error = L"import could not be bound";
                    return false;
                }
                *iat = reinterpret_cast<uint64_t>(target);
            }
        }
    }

    if (Nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].Size)
        HOST_LOG(L"  %s has delay-load imports; not bound yet", m_path.filename().c_str());
    return true;
}

void PeImage::ReadTls()
{
    auto const& dir = Nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (!dir.VirtualAddress) return;
    auto tls = At<IMAGE_TLS_DIRECTORY64>(dir.VirtualAddress);
    // Pointers in the TLS directory are VAs and were already relocated.
    if (tls->AddressOfCallBacks)
        for (auto cb = reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls->AddressOfCallBacks); *cb; ++cb)
            m_tlsCallbacks.push_back(*cb);
    if (tls->EndAddressOfRawData > tls->StartAddressOfRawData || tls->SizeOfZeroFill)
        HOST_LOG(L"  %s uses static TLS (__declspec(thread)); not supported yet, expect a crash if it's touched",
            m_path.filename().c_str());
}

bool PeImage::ProtectSections(std::wstring& error)
{
    ULONG old = 0;
    VirtualProtectFromApp(m_base, Nt()->OptionalHeader.SizeOfHeaders, PAGE_READONLY, &old);

    auto nt = Nt();
    auto section = IMAGE_FIRST_SECTION(nt);
    const size_t align = nt->OptionalHeader.SectionAlignment;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section)
    {
        size_t size = std::max<size_t>(section->Misc.VirtualSize, section->SizeOfRawData);
        size = (size + align - 1) / align * align;
        if (size == 0) continue;
        ULONG protect = SectionProtection(section->Characteristics);
        if (!VirtualProtectFromApp(m_base + section->VirtualAddress, size, protect, &old))
        {
            // Writable code isn't allowed everywhere; read-execute is the usual fallback.
            if (protect == PAGE_EXECUTE_READWRITE &&
                VirtualProtectFromApp(m_base + section->VirtualAddress, size, PAGE_EXECUTE_READ, &old))
                continue;
            wchar_t buf[128];
            swprintf_s(buf, L"can't set protection 0x%x on a section (error %u). Is codeGeneration declared?",
                protect, GetLastError());
            error = buf;
            return false;
        }
    }
    return true;
}

void PeImage::RegisterExceptionTable()
{
    auto const& dir = Nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (!dir.VirtualAddress || !dir.Size) return;
    auto table = At<RUNTIME_FUNCTION>(dir.VirtualAddress);
    if (RtlAddFunctionTable(table, dir.Size / sizeof(RUNTIME_FUNCTION), reinterpret_cast<DWORD64>(m_base)))
        m_functionTable = table;
    else
        HOST_LOG(L"  RtlAddFunctionTable failed for %s; exceptions won't unwind through it", m_path.filename().c_str());
}

void* PeImage::FindExport(const char* name) const
{
    auto const& dir = Nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto exp = At<IMAGE_EXPORT_DIRECTORY>(dir.VirtualAddress);
    auto names = At<uint32_t>(exp->AddressOfNames);
    auto ordinals = At<uint16_t>(exp->AddressOfNameOrdinals);
    for (DWORD i = 0; i < exp->NumberOfNames; ++i)
        if (strcmp(At<const char>(names[i]), name) == 0)
            return FindExportByOrdinal(static_cast<uint16_t>(ordinals[i] + exp->Base));
    return nullptr;
}

void* PeImage::FindExportByOrdinal(uint16_t ordinal) const
{
    auto const& dir = Nt()->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!dir.VirtualAddress) return nullptr;
    auto exp = At<IMAGE_EXPORT_DIRECTORY>(dir.VirtualAddress);
    uint32_t index = ordinal - exp->Base;
    if (ordinal < exp->Base || index >= exp->NumberOfFunctions) return nullptr;
    uint32_t rva = At<uint32_t>(exp->AddressOfFunctions)[index];
    if (rva >= dir.VirtualAddress && rva < dir.VirtualAddress + dir.Size)
    {
        HOST_LOG(L"  forwarded export in %s not supported yet: %S", m_path.filename().c_str(), At<const char>(rva));
        return nullptr;
    }
    return rva ? m_base + rva : nullptr;
}
