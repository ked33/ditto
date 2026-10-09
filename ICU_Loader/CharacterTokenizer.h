#pragma once

// Included after sqlite3ext.h and ICU. The loader's historical SQLite header
// predates the FTS5 declarations. Use only the stable registration ABI prefix
// (available since SQLite 3.20), without importing a second SQLite header.
namespace DittoCharacterTokenizer
{
    struct Tokenizer;
    struct Methods
    {
        int (*xCreate)(void*, const char**, int, Tokenizer**);
        void (*xDelete)(Tokenizer*);
        int (*xTokenize)(Tokenizer*, void*, int, const char*, int,
            int (*)(void*, int, const char*, int, int, int));
    };

    struct Api
    {
        int iVersion;
        int (*xCreateTokenizer)(Api*, const char*, void*, Methods*, void (*)(void*));
    };

    struct Tokenizer { unsigned char unused; };

    static int Create(void*, const char**, int count, Tokenizer** output)
    {
        *output = nullptr;
        if (count != 0)
            return SQLITE_ERROR;
        *output = static_cast<Tokenizer*>(sqlite3_malloc(sizeof(Tokenizer)));
        return *output ? SQLITE_OK : SQLITE_NOMEM;
    }

    static void Delete(Tokenizer* tokenizer)
    {
        sqlite3_free(tokenizer);
    }

    static int Tokenize(Tokenizer*, void* context, int, const char* text, int length,
        int (*emit)(void*, int, const char*, int, int, int))
    {
        int offset = 0;
        while (offset < length && text[offset] != '\0')
        {
            const int start = offset;
            UChar32 codepoint;
            U8_NEXT(text, offset, length, codepoint);
            if (codepoint < 0)
                return SQLITE_ERROR;

            // Match icuLikeCompare exactly: simple Unicode case folding, no
            // accent removal, word splitting, or whitespace normalization.
            codepoint = u_foldCase(codepoint, U_FOLD_CASE_DEFAULT);
            char token[U8_MAX_LENGTH];
            int tokenLength = 0;
            U8_APPEND_UNSAFE(token, tokenLength, codepoint);
            const int rc = emit(context, 0, token, tokenLength, start, offset);
            if (rc != SQLITE_OK)
                return rc;
        }
        return SQLITE_OK;
    }

    static int Register(sqlite3* db)
    {
        Api* api = nullptr;
        sqlite3_stmt* statement = nullptr;
        int rc = sqlite3_prepare_v2(db, "SELECT fts5(?1)", -1, &statement, nullptr);
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_pointer(statement, 1, &api, "fts5_api_ptr", nullptr);
        if (rc == SQLITE_OK)
        {
            const int step = sqlite3_step(statement);
            rc = step == SQLITE_ROW ? SQLITE_OK : step;
        }
        const int finalRc = sqlite3_finalize(statement);
        if (rc != SQLITE_OK)
            return rc;
        if (finalRc != SQLITE_OK)
            return finalRc;
        if (api == nullptr || api->iVersion < 2)
            return SQLITE_ERROR;

        Methods methods = { Create, Delete, Tokenize };
        return api->xCreateTokenizer(api, "ditto_char", nullptr, &methods, nullptr);
    }
}
