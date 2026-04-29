#ifndef DG_JSMN_H
#define DG_JSMN_H

typedef enum {
    JSMN_UNDEFINED = 0,
    JSMN_OBJECT = 1,
    JSMN_ARRAY = 2,
    JSMN_STRING = 3,
    JSMN_PRIMITIVE = 4,
} jsmntype_t;

typedef enum {
    JSMN_ERROR_NOMEM = -1,
    JSMN_ERROR_INVAL = -2,
    JSMN_ERROR_PART = -3,
} jsmnerr_t;

typedef struct {
    jsmntype_t type;
    int start;
    int end;
    int size;
    int parent;
} jsmntok_t;

typedef struct {
    unsigned int pos;
    unsigned int toknext;
    int toksuper;
} jsmn_parser;

static void jsmn_init(jsmn_parser *parser) {
    parser->pos = 0;
    parser->toknext = 0;
    parser->toksuper = -1;
}

static jsmntok_t *jsmn_alloc_token(jsmn_parser *parser, jsmntok_t *tokens, size_t num_tokens) {
    jsmntok_t *tok;

    if (parser->toknext >= num_tokens) {
        return 0;
    }

    tok = &tokens[parser->toknext++];
    tok->start = -1;
    tok->end = -1;
    tok->size = 0;
    tok->parent = -1;
    tok->type = JSMN_UNDEFINED;
    return tok;
}

static void jsmn_fill_token(jsmntok_t *token, jsmntype_t type, int start, int end) {
    token->type = type;
    token->start = start;
    token->end = end;
    token->size = 0;
}

static int jsmn_parse_primitive(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens,
                                size_t num_tokens) {
    int start = (int)parser->pos;
    jsmntok_t *token;

    while (parser->pos < len) {
        char c = js[parser->pos];
        if (c == '\t' || c == '\r' || c == '\n' || c == ' ' || c == ',' || c == ']' || c == '}') {
            break;
        }
        if (c < 32 || c >= 127) {
            return JSMN_ERROR_INVAL;
        }
        parser->pos++;
    }

    if (tokens == 0) {
        parser->pos--;
        return 0;
    }

    token = jsmn_alloc_token(parser, tokens, num_tokens);
    if (token == 0) {
        parser->pos = (unsigned int)start;
        return JSMN_ERROR_NOMEM;
    }

    jsmn_fill_token(token, JSMN_PRIMITIVE, start, (int)parser->pos);
    token->parent = parser->toksuper;
    if (parser->toksuper >= 0) {
        tokens[parser->toksuper].size++;
    }

    parser->pos--;
    return 0;
}

static int jsmn_parse_string(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens,
                             size_t num_tokens) {
    int start = (int)parser->pos;
    jsmntok_t *token;

    parser->pos++;

    while (parser->pos < len) {
        char c = js[parser->pos];
        if (c == '"') {
            if (tokens == 0) {
                return 0;
            }

            token = jsmn_alloc_token(parser, tokens, num_tokens);
            if (token == 0) {
                parser->pos = (unsigned int)start;
                return JSMN_ERROR_NOMEM;
            }

            jsmn_fill_token(token, JSMN_STRING, start + 1, (int)parser->pos);
            token->parent = parser->toksuper;
            if (parser->toksuper >= 0) {
                tokens[parser->toksuper].size++;
            }
            return 0;
        }

        if (c == '\\') {
            parser->pos++;
            if (parser->pos >= len) {
                return JSMN_ERROR_PART;
            }
            if (js[parser->pos] == 'u') {
                int i;
                for (i = 0; i < 4; i++) {
                    parser->pos++;
                    if (parser->pos >= len) {
                        return JSMN_ERROR_PART;
                    }
                }
            }
        }

        parser->pos++;
    }

    return JSMN_ERROR_PART;
}

static int jsmn_parse(jsmn_parser *parser, const char *js, size_t len, jsmntok_t *tokens,
                      unsigned int num_tokens) {
    int r;
    int i;

    for (; parser->pos < len; parser->pos++) {
        char c = js[parser->pos];
        jsmntok_t *token;

        switch (c) {
            case '{':
            case '[':
                token = jsmn_alloc_token(parser, tokens, num_tokens);
                if (token == 0) {
                    return JSMN_ERROR_NOMEM;
                }
                token->type = (c == '{') ? JSMN_OBJECT : JSMN_ARRAY;
                token->start = (int)parser->pos;
                token->parent = parser->toksuper;
                if (parser->toksuper >= 0) {
                    tokens[parser->toksuper].size++;
                }
                parser->toksuper = (int)parser->toknext - 1;
                break;

            case '}':
            case ']':
                if (tokens == 0) {
                    break;
                }
                for (i = (int)parser->toknext - 1; i >= 0; i--) {
                    token = &tokens[i];
                    if (token->start != -1 && token->end == -1) {
                        if ((token->type == JSMN_OBJECT && c == '}') ||
                            (token->type == JSMN_ARRAY && c == ']')) {
                            token->end = (int)parser->pos + 1;
                            parser->toksuper = token->parent;
                            break;
                        }
                        return JSMN_ERROR_INVAL;
                    }
                }
                if (i == -1) {
                    return JSMN_ERROR_INVAL;
                }
                break;

            case '"':
                r = jsmn_parse_string(parser, js, len, tokens, num_tokens);
                if (r < 0) {
                    return r;
                }
                break;

            case '\t':
            case '\r':
            case '\n':
            case ' ':
            case ':':
            case ',':
                break;

            default:
                r = jsmn_parse_primitive(parser, js, len, tokens, num_tokens);
                if (r < 0) {
                    return r;
                }
                break;
        }
    }

    for (i = (int)parser->toknext - 1; i >= 0; i--) {
        if (tokens[i].start != -1 && tokens[i].end == -1) {
            return JSMN_ERROR_PART;
        }
    }

    return (int)parser->toknext;
}

#endif