#pragma once

// Maps one x64 PE file (an exe or DLL) into memory the way the Windows loader
// would: sections at their RVAs, base relocations applied, imports bound
// through the resolver, page protections set, and its exception table
// registered so C++ exceptions and SEH can unwind through it.
class ImportResolver
{
public:
    virtual ~ImportResolver() = default;
    // name is null when importing by ordinal. Never returns null: anything
    // unknown comes back as a logging stub.
    virtual void* ResolveImport(const char* dll, const char* name, uint16_t ordinal) = 0;
};

class PeImage
{
public:
    PeImage() = default;
    PeImage(PeImage const&) = delete;
    PeImage& operator=(PeImage const&) = delete;
    ~PeImage();

    // Returns false and fills error on failure.
    bool Load(std::filesystem::path const& path, ImportResolver& resolver, std::wstring& error);

    uint8_t* Base() const { return m_base; }
    size_t Size() const { return m_size; }
    void* EntryPoint() const { return m_entry; }
    bool IsDll() const { return m_isDll; }
    bool Relocated() const { return m_relocated; }
    std::filesystem::path const& Path() const { return m_path; }
    bool Contains(const void* p) const { return p >= m_base && p < m_base + m_size; }

    void* FindExport(const char* name) const;
    void* FindExportByOrdinal(uint16_t ordinal) const;

    // TLS callbacks to run on thread attach/detach, in order.
    std::vector<PIMAGE_TLS_CALLBACK> const& TlsCallbacks() const { return m_tlsCallbacks; }

private:
    bool ApplyRelocations(intptr_t delta, std::wstring& error);
    bool BindImports(ImportResolver& resolver, std::wstring& error);
    void ReadTls();
    bool ProtectSections(std::wstring& error);
    void RegisterExceptionTable();
    template <typename T> T* At(uint32_t rva) const { return reinterpret_cast<T*>(m_base + rva); }
    IMAGE_NT_HEADERS64* Nt() const;

    std::filesystem::path m_path;
    uint8_t* m_base = nullptr;
    size_t m_size = 0;
    void* m_entry = nullptr;
    bool m_isDll = false;
    bool m_relocated = false;
    PRUNTIME_FUNCTION m_functionTable = nullptr;
    std::vector<PIMAGE_TLS_CALLBACK> m_tlsCallbacks;
};
