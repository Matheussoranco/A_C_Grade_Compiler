#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef enum {
    TOKEN_EOF,
    TOKEN_IDENTIFIER,
    TOKEN_NUMBER,
    TOKEN_STRING,
    TOKEN_KEYWORD,
    TOKEN_OPERATOR,
    TOKEN_PUNCTUATION
} TokenType;

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

char lexer_peek(Lexer* lexer) {
    if (lexer->pos >= lexer->source_len){
        return '\0';
    }
    return lexer->source[lexer->pos];
}

void lexer_advance(Lexer* lexer) {
    if(lexer->pos < lexer->source_len) {
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
    while (isspace(lexer_peek(lexer))) {
        lexer_advance(lexer);
    }
    
    if (lexer_peek(lexer) == '\0') {
        Token* token = malloc(sizeof(Token));
        token->type = TOKEN_EOF;
        token->value = NULL;
        token->line = lexer->line;
        token->column = lexer->column;
        return token;
    }
    
    // Implementar lógica para identificar outros tipos de tokens
    // (números, strings, identificadores, operadores, etc.)
   
    // Placeholder - implementação básica
    Token* token = malloc(sizeof(Token));
    token->type = TOKEN_IDENTIFIER;
    token->value = strndup(lexer->source + lexer->pos, 1);
    token->line = lexer->line;
    token->column = lexer->column;
    lexer_advance(lexer);
    return token;
}

void free_token(Token* token) {
    if (token->value != NULL) {
        free(token->value);
    }
    free(token);
}

int main(int argc, char** argv) {
    if(argc < 2) {
        printf("Uso: %s <arquivo_fonte>\n", argv[0]);
        return 1;
    }
    
    // Ler arquivo de entrada
    FILE* file = fopen(argv[1], "r");
    if (!file) {
        perror("Erro ao abrir arquivo");
        return 1;
    }
    
    fseek(file, 0, SEEK_END);
    long file_size = ftell(file);
    fseek(file, 0, SEEK_SET);
    
    char* source = malloc(file_size + 1);
    fread(source, 1, file_size, file);
    source[file_size] = '\0';
    fclose(file);
    
    // Processar código fonte
    Lexer* lexer = init_lexer(source);
    Token* token = NULL;
    
    printf("Tokens encontrados:\n");
    do {
        token = lexer_next_token(lexer);
        printf("Token [%d:%d] Tipo: %d, Valor: %s\n",
                token->line, token->column, token->type,
                token->value ? token->value : "NULL");
        
        TokenType current_type = token->type;
        free_token(token);
        
        if (current_type == TOKEN_EOF) break;
        
    } while (1);
    
    free(source);
    free_lexer(lexer);
    return 0;
}