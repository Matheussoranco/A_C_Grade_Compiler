#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

typedef enum {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_INTEGER,
    TOKEN_FLOAT,
    TOKEN_STRING,
    TOKEN_CHAR,
    TOKEN_KEYWORD,
    TOKEN_OPERATOR,
    TOKEN_PUNCTUATION,
    TOKEN_COMMENT,
    TOKEN_PREPROCESSOR
} TokenType;

const char* KEYWORDS[] = {
    "if", "else", "while", "for", "return",
    "int", "float", "char", "void", "struct",
    "typedef", "#include", "#define", NULL
};

const char* OPERATORS[] = {
    "+", "-", "*", "/", "%", "=", "==", "!=", "<", ">", 
    "<=", ">=", "&&", "||", "!", "&", "|", "^", "~", 
    "<<", ">>", "++", "--", "+=", "-=", "*=", "/=", "%=", 
    NULL
};

const char* PUNCTUATION[] = {
    ";", ",", ".", ":", "(", ")", "{", "}", "[", "]", NULL
};

typedef struct {
    TokenType type;
    char* value;
    int line;
    int column;
} Token;

typedef struct {
    char* source;
    int source_len;
    int pos;
    int line;
    int column;
} Lexer;

bool is_keyword(const char* str) {
    for (int i = 0; KEYWORDS[i] != NULL; i++) {
        if (strcmp(str, KEYWORDS[i]) == 0) return true;
    }
    return false;
}

bool is_operator(const char* str) {
    for (int i = 0; OPERATORS[i] != NULL; i++) {
        if (strcmp(str, OPERATORS[i]) == 0) return true;
    }
    return false;
}

bool is_punctuation(char c) {
    for (int i = 0; PUNCTUATION[i] != NULL; i++) {
        if (c == PUNCTUATION[i][0]) return true;
    }
    return false;
}

Lexer* init_lexer(char* source) {
    Lexer* lexer = malloc(sizeof(Lexer));
    lexer->source = source;
    lexer->source_len = strlen(source);
    lexer->pos = 0;
    lexer->line = 1;
    lexer->column = 1;
    return lexer;
}

void free_lexer(Lexer* lexer) {
    free(lexer);
}

char lexer_peek(Lexer* lexer, int ahead) {
    if (lexer->pos + ahead >= lexer->source_len) {
        return '\0';
    }
    return lexer->source[lexer->pos + ahead];
}

void lexer_advance(Lexer* lexer) {
    if (lexer->pos < lexer->source_len) {
        if (lexer->source[lexer->pos] == '\n') {
            lexer->line++;
            lexer->column = 1;
        } else {
            lexer->column++;
        }
        lexer->pos++;
    }
}

Token* lexer_next_token(Lexer* lexer) {
    while (isspace(lexer_peek(lexer, 0))) {
        lexer_advance(lexer);
    }

    if (lexer_peek(lexer, 0) == '\0') {
        Token* token = malloc(sizeof(Token));
        token->type = TOKEN_EOF;
        token->value = NULL;
        token->line = lexer->line;
        token->column = lexer->column;
        return token;
    }

    if (lexer_peek(lexer, 0) == '/' && lexer_peek(lexer, 1) == '/') {
        while (lexer_peek(lexer, 0) != '\n' && lexer_peek(lexer, 0) != '\0') {
            lexer_advance(lexer);
        }
        return lexer_next_token(lexer);
    }

    if (lexer_peek(lexer, 0) == '/' && lexer_peek(lexer, 1) == '*') {
        lexer_advance(lexer);
        lexer_advance(lexer);
        while (!(lexer_peek(lexer, 0) == '*' && lexer_peek(lexer, 1) == '/')) {
            if (lexer_peek(lexer, 0) == '\0') {
                fprintf(stderr, "Erro: Comentário não fechado\n");
                exit(1);
            }
            lexer_advance(lexer);
        }
        lexer_advance(lexer);
        lexer_advance(lexer);
        return lexer_next_token(lexer);
    }

    if (lexer->column == 1 && lexer_peek(lexer, 0) == '#') {
        int start = lexer->pos;
        while (isalpha(lexer_peek(lexer, 0)) || lexer_peek(lexer, 0) == '_') {
            lexer_advance(lexer);
        }
        char* value = strndup(lexer->source + start, lexer->pos - start);
        
        Token* token = malloc(sizeof(Token));
        token->type = TOKEN_PREPROCESSOR;
        token->value = value;
        token->line = lexer->line;
        token->column = lexer->column;
        return token;
    }

    if (lexer_peek(lexer, 0) == '"') {
        lexer_advance(lexer);
        int start = lexer->pos;
        while (lexer_peek(lexer, 0) != '"' && lexer_peek(lexer, 0) != '\0') {
            if (lexer_peek(lexer, 0) == '\\') {
                lexer_advance(lexer);
            }
            lexer_advance(lexer);
        }
        
        if (lexer_peek(lexer, 0) == '\0') {
            fprintf(stderr, "Erro: String não fechada\n");
            exit(1);
        }
        
        char* value = strndup(lexer->source + start, lexer->pos - start);
        lexer_advance(lexer);
        
        Token* token = malloc(sizeof(Token));
        token->type = TOKEN_STRING;
        token->value = value;
        token->line = lexer->line;
        token->column = lexer->column;
        return token;
    }

    if (lexer_peek(lexer, 0) == '\'') {
        lexer_advance(lexer);
        int start = lexer->pos;
        
        if (lexer_peek(lexer, 0) == '\\') {
            lexer_advance(lexer);
        }
        lexer_advance(lexer);
        
        if (lexer_peek(lexer, 0) != '\'') {
            fprintf(stderr, "Erro: Caractere não fechado\n");
            exit(1);
        }
        
        char* value = strndup(lexer->source + start, lexer->pos - start);
        lexer_advance(lexer);
        
        Token* token = malloc(sizeof(Token));
        token->type = TOKEN_CHAR;
        token->value = value;
        token->line = lexer->line;
        token->column = lexer->column;
        return token;
    }

    if (isdigit(lexer_peek(lexer, 0))) {
        int start = lexer->pos;
        bool is_float = false;
        
        while (isdigit(lexer_peek(lexer, 0))) {
            lexer_advance(lexer);
        }
        
        if (lexer_peek(lexer, 0) == '.') {
            is_float = true;
            lexer_advance(lexer);
            while (isdigit(lexer_peek(lexer, 0))) {
                lexer_advance(lexer);
            }
        }
        
        char* value = strndup(lexer->source + start, lexer->pos - start);
        
        Token* token = malloc(sizeof(Token));
        token->type = is_float ? TOKEN_FLOAT : TOKEN_INTEGER;
        token->value = value;
        token->line = lexer->line;
        token->column = lexer->column;
        return token;
    }

    if (isalpha(lexer_peek(lexer, 0)) || lexer_peek(lexer, 0) == '_') {
        int start = lexer->pos;
        while (isalnum(lexer_peek(lexer, 0)) || lexer_peek(lexer, 0) == '_') {
            lexer_advance(lexer);
        }
        
        char* value = strndup(lexer->source + start, lexer->pos - start);
        
        Token* token = malloc(sizeof(Token));
        token->type = is_keyword(value) ? TOKEN_KEYWORD : TOKEN_IDENTIFIER;
        token->value = value;
        token->line = lexer->line;
        token->column = lexer->column;
        return token;
    }

    for (int op_len = 3; op_len > 0; op_len--) {
        if (lexer->pos + op_len > lexer->source_len) continue;
        
        char op[4] = {0};
        strncpy(op, lexer->source + lexer->pos, op_len);
        
        if (is_operator(op)) {
            lexer->pos += op_len;
            lexer->column += op_len;
            
            Token* token = malloc(sizeof(Token));
            token->type = TOKEN_OPERATOR;
            token->value = strdup(op);
            token->line = lexer->line;
            token->column = lexer->column;
            return token;
        }
    }

    if (is_punctuation(lexer_peek(lexer, 0))) {
        char punc[2] = {lexer_peek(lexer, 0), '\0'};
        lexer_advance(lexer);
        
        Token* token = malloc(sizeof(Token));
        token->type = TOKEN_PUNCTUATION;
        token->value = strdup(punc);
        token->line = lexer->line;
        token->column = lexer->column;
        return token;
    }

    fprintf(stderr, "Erro léxico: Caractere inesperado '%c' na linha %d, coluna %d\n",
            lexer_peek(lexer, 0), lexer->line, lexer->column);
    exit(1);
}

void free_token(Token* token) {
    if (token->value != NULL) {
        free(token->value);
    }
    free(token);
}