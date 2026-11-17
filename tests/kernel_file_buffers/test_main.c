#include "kernel.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DWORD captured_flags;
static WCHAR fixture_path[MAX_PATH];

static HANDLE capture_create_file(LPCWSTR name, DWORD access, DWORD share,
    LPSECURITY_ATTRIBUTES security, DWORD disposition, DWORD flags, HANDLE template_file)
{
    captured_flags = flags;
    return CreateFileW(name, access, share, security, disposition, flags, template_file);
}

/* Observe the actual host open flags; all reads/writes use real Windows I/O. */
#define CreateFileW capture_create_file
#include KERNEL_FILE_TEST_SOURCE
#undef CreateFileW

BOOL xbox_translate_path(const char *path, WCHAR *out, DWORD size)
{
    if (strcmp(path, "D:\\InGameOn.bin") || wcslen(fixture_path) >= size) return FALSE;
    wcscpy(out, fixture_path);
    return TRUE;
}

void xbox_log(int level, const char *subsystem, const char *format, ...)
{
    (void)level; (void)subsystem; (void)format;
}

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL line %d: %s (Windows error %lu)\n", __LINE__, #condition, GetLastError()); \
    failed = 1; goto done; } } while (0)

int main(void)
{
    int failed = 0;
    HANDLE file = INVALID_HANDLE_VALUE, event = NULL;
    unsigned char *storage = NULL, *expected = NULL;
    WCHAR temp[MAX_PATH];
    DWORD written = 0;
    const DWORD asset_size = 229664, disc_request = 231424;
    XBOX_ANSI_STRING name = {15, 16, "D:\\InGameOn.bin"};
    XBOX_OBJECT_ATTRIBUTES attributes = {0};
    XBOX_IO_STATUS_BLOCK io = {0};
    LARGE_INTEGER offset;
    attributes.ObjectName = &name;

    CHECK(GetTempPathW(MAX_PATH, temp));
    CHECK(GetTempFileNameW(temp, L"xrf", 0, fixture_path));
    expected = malloc(asset_size);
    storage = VirtualAlloc(NULL, disc_request + 8192, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    CHECK(expected && storage);
    for (DWORD i = 0; i < asset_size; ++i) expected[i] = (unsigned char)(i * 37u + i / 251u);
    file = CreateFileW(fixture_path, GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    CHECK(file != INVALID_HANDLE_VALUE);
    CHECK(WriteFile(file, expected, asset_size, &written, NULL) && written == asset_size);
    CHECK(CloseHandle(file)); file = INVALID_HANDLE_VALUE;

    for (unsigned uncached = 0; uncached < 2; ++uncached) {
        ULONG options = uncached ? XBOX_FILE_NO_INTERMEDIATE_BUFFERING : 0;
        CHECK(xbox_NtCreateFile(&file, XBOX_GENERIC_READ, &attributes, &io, NULL,
            0, 1, XBOX_FILE_OPEN, options) == STATUS_SUCCESS);
        CHECK(io.Status == STATUS_SUCCESS);
        CHECK(!(captured_flags & FILE_FLAG_NO_BUFFERING));

        /* The report's 113-disc-sector request, into a 2K-aligned buffer that
         * deliberately does not meet the 4K host-sector alignment. */
        unsigned char *buffer = storage + 2048;
        offset.QuadPart = 0;
        memset(buffer, 0xCC, disc_request);
        CHECK(xbox_NtReadFile(file, NULL, NULL, NULL, &io, buffer, disc_request, &offset) == STATUS_SUCCESS);
        CHECK(io.Status == STATUS_SUCCESS && io.Information == asset_size);
        CHECK(!memcmp(buffer, expected, asset_size));
        CHECK(buffer[asset_size] == 0xCC);

        event = CreateEventW(NULL, TRUE, FALSE, NULL);
        CHECK(event);
        offset.QuadPart = 2048;
        CHECK(xbox_NtReadFile(file, event, NULL, NULL, &io, buffer, 6144, &offset) == STATUS_SUCCESS);
        CHECK(io.Information == 6144 && !memcmp(buffer, expected + 2048, 6144));
        CHECK(WaitForSingleObject(event, 0) == WAIT_OBJECT_0);
        CHECK(CloseHandle(event)); event = NULL;

        /* Also reject any accidental dependence on host I/O alignment. */
        offset.QuadPart = 1;
        CHECK(xbox_NtReadFile(file, NULL, NULL, NULL, &io, storage + 1, 31, &offset) == STATUS_SUCCESS);
        CHECK(io.Information == 31 && !memcmp(storage + 1, expected + 1, 31));
        offset.QuadPart = asset_size;
        CHECK(xbox_NtReadFile(file, NULL, NULL, NULL, &io, buffer, 2048, &offset) == STATUS_END_OF_FILE);
        CHECK(io.Status == STATUS_END_OF_FILE && io.Information == 0);
        CHECK(xbox_NtClose(file) == STATUS_SUCCESS); file = INVALID_HANDLE_VALUE;
    }
done:
    if (event) CloseHandle(event);
    if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
    if (storage) VirtualFree(storage, 0, MEM_RELEASE);
    free(expected);
    if (fixture_path[0]) DeleteFileW(fixture_path);
    if (!failed) puts("PASS: cached host flags, Xbox sector reads, short reads, offsets, events and EOF");
    return failed;
}
