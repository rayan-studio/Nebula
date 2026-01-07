#pragma once
#include <dwrite.h>
#include <vector>
#include <string>

class CustomFontFileStream : public IDWriteFontFileStream
{
private:
    ULONG refCount_;
    std::vector<BYTE> fontData_;

public:
    CustomFontFileStream(const std::vector<BYTE> &data)
        : refCount_(1), fontData_(data) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontFileStream))
        {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = --refCount_;
        if (count == 0)
            delete this;
        return count;
    }

    // IDWriteFontFileStream
    HRESULT STDMETHODCALLTYPE ReadFileFragment(
        void const **fragmentStart,
        UINT64 fileOffset,
        UINT64 fragmentSize,
        void **fragmentContext) override
    {
        if (fileOffset + fragmentSize > fontData_.size())
            return E_FAIL;

        *fragmentStart = fontData_.data() + fileOffset;
        *fragmentContext = nullptr;
        return S_OK;
    }

    void STDMETHODCALLTYPE ReleaseFileFragment(void *fragmentContext) override
    {
        (void)fragmentContext;
    }

    HRESULT STDMETHODCALLTYPE GetFileSize(UINT64 *fileSize) override
    {
        *fileSize = fontData_.size();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetLastWriteTime(UINT64 *lastWriteTime) override
    {
        *lastWriteTime = 0;
        return S_OK;
    }
};

class CustomFontFileLoader : public IDWriteFontFileLoader
{
private:
    ULONG refCount_;
    std::vector<BYTE> fontData_;

public:
    CustomFontFileLoader() : refCount_(1) {}

    bool LoadFontFile(const std::wstring &path)
    {
        HANDLE hFile = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                   NULL, OPEN_EXISTING, 0, NULL);
        if (hFile == INVALID_HANDLE_VALUE)
            return false;

        DWORD size = GetFileSize(hFile, NULL);
        fontData_.resize(size);

        DWORD read = 0;
        bool success = ReadFile(hFile, fontData_.data(), size, &read, NULL) && (read == size);
        CloseHandle(hFile);

        return success;
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontFileLoader))
        {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = --refCount_;
        if (count == 0)
            delete this;
        return count;
    }

    // IDWriteFontFileLoader
    HRESULT STDMETHODCALLTYPE CreateStreamFromKey(
        void const *fontFileReferenceKey,
        UINT32 fontFileReferenceKeySize,
        IDWriteFontFileStream **fontFileStream) override
    {
        (void)fontFileReferenceKey;
        (void)fontFileReferenceKeySize;
        *fontFileStream = new CustomFontFileStream(fontData_);
        return S_OK;
    }
};

// Enumerator over one or more IDWriteFontFile pointers
class CustomFontFileEnumerator : public IDWriteFontFileEnumerator
{
private:
    ULONG refCount_;
    std::vector<IDWriteFontFile *> files_;
    UINT32 index_;

public:
    CustomFontFileEnumerator(const std::vector<IDWriteFontFile *> &files)
        : refCount_(1), files_(files), index_(0xFFFFFFFF)
    {
        for (auto f : files_)
            if (f)
                f->AddRef();
    }

    ~CustomFontFileEnumerator()
    {
        for (auto f : files_)
            if (f)
                f->Release();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontFileEnumerator))
        {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = --refCount_;
        if (count == 0)
            delete this;
        return count;
    }

    // IDWriteFontFileEnumerator
    HRESULT STDMETHODCALLTYPE MoveNext(BOOL *hasCurrent) override
    {
        if (!hasCurrent)
            return E_INVALIDARG;
        if (files_.empty())
        {
            *hasCurrent = FALSE;
            return S_OK;
        }
        if (index_ == 0xFFFFFFFF)
            index_ = 0;
        else
            index_++;

        *hasCurrent = (index_ < files_.size()) ? TRUE : FALSE;
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE GetCurrentFontFile(IDWriteFontFile **fontFile) override
    {
        if (!fontFile)
            return E_INVALIDARG;
        if (index_ == 0xFFFFFFFF || index_ >= files_.size())
            return E_FAIL;
        *fontFile = files_[index_];
        if (*fontFile)
            (*fontFile)->AddRef();
        return S_OK;
    }
};

// Collection loader that produces an enumerator from a collection key (we use the key as a wchar_t path)
class CustomFontCollectionLoader : public IDWriteFontCollectionLoader
{
private:
    ULONG refCount_;

public:
    CustomFontCollectionLoader() : refCount_(1) {}

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppvObject) override
    {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWriteFontCollectionLoader))
        {
            *ppvObject = this;
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }

    ULONG STDMETHODCALLTYPE AddRef() override { return ++refCount_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        ULONG count = --refCount_;
        if (count == 0)
            delete this;
        return count;
    }

    // IDWriteFontCollectionLoader
    HRESULT STDMETHODCALLTYPE CreateEnumeratorFromKey(
        IDWriteFactory *factory,
        void const *collectionKey,
        UINT32 collectionKeySize,
        IDWriteFontFileEnumerator **fontFileEnumerator) override
    {
        if (!factory || !collectionKey || !fontFileEnumerator)
            return E_INVALIDARG;

        // Treat the key as a wide string path (null-terminated)
        const wchar_t *path = reinterpret_cast<const wchar_t *>(collectionKey);
        (void)collectionKeySize;

        std::vector<IDWriteFontFile *> files;
        IDWriteFontFile *fontFile = nullptr;
        HRESULT hr = factory->CreateFontFileReference(path, nullptr, &fontFile);
        if (SUCCEEDED(hr) && fontFile)
        {
            files.push_back(fontFile);
            // Do NOT release here: the enumerator will AddRef() in its constructor
            // releasing here would delete the object before the enumerator can addref it.
        }

        *fontFileEnumerator = new CustomFontFileEnumerator(files);
        return S_OK;
    }
};