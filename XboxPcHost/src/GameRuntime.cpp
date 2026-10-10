#include "pch.h"
#include "GameRuntime.h"
#include "Log.h"

namespace
{
    const wchar_t* MachineName(WORD machine)
    {
        switch (machine)
        {
        case IMAGE_FILE_MACHINE_AMD64: return L"x64";
        case IMAGE_FILE_MACHINE_I386: return L"x86";
        case IMAGE_FILE_MACHINE_ARM64: return L"ARM64";
        default: return L"unknown";
        }
    }

    // Placeholder until the loader exists: it opens the exe, checks the PE
    // headers, reports what it found, and declines to run it.
    class StubRuntime final : public IGameRuntime
    {
    public:
        bool Start(GameEntry const& game, HostServices&) override
        {
            m_message = Inspect(game.exe);
            HOST_LOG(L"%s: %s", game.name.c_str(), m_message.c_str());
            return false;
        }
        void Tick() override {}
        void Stop() override {}
        bool IsRunning() const override { return false; }
        std::wstring const& LastMessage() const override { return m_message; }

    private:
        static std::wstring Inspect(std::filesystem::path const& exe)
        {
            FILE* f = nullptr;
            if (_wfopen_s(&f, exe.c_str(), L"rb") != 0 || !f)
                return L"can't open exe";

            IMAGE_DOS_HEADER dos{};
            IMAGE_NT_HEADERS64 nt{};
            bool ok = fread(&dos, sizeof(dos), 1, f) == 1 && dos.e_magic == IMAGE_DOS_SIGNATURE &&
                fseek(f, dos.e_lfanew, SEEK_SET) == 0 && fread(&nt, sizeof(nt), 1, f) == 1 &&
                nt.Signature == IMAGE_NT_SIGNATURE;
            fclose(f);
            if (!ok) return L"not a valid PE file";

            wchar_t buf[256];
            if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64)
            {
                swprintf_s(buf, L"%s exe; only x64 games can run on the console", MachineName(nt.FileHeader.Machine));
                return buf;
            }
            swprintf_s(buf, L"valid x64 PE, %u sections, image size %u KB. Loader not built yet (stage 2).",
                nt.FileHeader.NumberOfSections, nt.OptionalHeader.SizeOfImage / 1024);
            return buf;
        }

        std::wstring m_message;
    };
}

std::unique_ptr<IGameRuntime> CreateGameRuntime()
{
    return std::make_unique<StubRuntime>();
}
