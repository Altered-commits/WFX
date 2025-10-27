/* Check lexer.hpp first */
#ifndef UNNAMED_TOKEN_HPP
#define UNNAMED_TOKEN_HPP

#include <unordered_map>
#include <string>

namespace WFX::Core::Legacy {

enum TokenType : std::uint8_t {
    //Primitive Types
    TOKEN_INT,
    TOKEN_FLOAT,
    TOKEN_STRING,
    //() Type
    TOKEN_LPAREN,
    TOKEN_RPAREN,
    //{} Type,
    TOKEN_LBRACE,
    TOKEN_RBRACE,
    //<> Type
    TOKEN_LT,
    TOKEN_GT,
    //<=, >= Type
    TOKEN_LTEQ,
    TOKEN_GTEQ,
    //Arithmetic Types
    TOKEN_PLUS,
    TOKEN_MINUS,
    TOKEN_MULT,
    TOKEN_DIV,
    TOKEN_MODULO,
    TOKEN_POW,
    //Assignment Type
    TOKEN_EQ,
    //==, != Type
    TOKEN_EEQ,
    TOKEN_NEQ,
    //&&, !, || Type
    TOKEN_AND,
    TOKEN_NOT,
    TOKEN_OR,
    //Keyword and Identifier Types
    TOKEN_ID,
    TOKEN_KEYWORD_AUTO,
    TOKEN_KEYWORD_VOID,
    TOKEN_KEYWORD_INT,
    TOKEN_KEYWORD_FLOAT,
    TOKEN_KEYWORD_CAST,
    TOKEN_KEYWORD_IS,
    TOKEN_KEYWORD_IF,
    TOKEN_KEYWORD_ELIF,
    TOKEN_KEYWORD_ELSE,
    TOKEN_KEYWORD_FOR,
    TOKEN_KEYWORD_IN,
    TOKEN_KEYWORD_WHILE,
    TOKEN_KEYWORD_FUNC,
    TOKEN_KEYWORD_CONTINUE,
    TOKEN_KEYWORD_BREAK,
    TOKEN_KEYWORD_RETURN,
    //, Type
    TOKEN_COMMA,
    //Ternary(? :) Type,
    TOKEN_QUESTION,
    TOKEN_COLON,
    //Statement End
    TOKEN_SEMIC,
    TOKEN_EOF,
    //. and .. and ... (Dot, Range and Ellipsis)
    TOKEN_DOT,
    TOKEN_RANGE,
    TOKEN_ELLIPSIS,
};

//Some useful string repr of token type
//Mostly used in printing of error messages
const std::unordered_map<TokenType, const char* const> token_type_to_string = {
    { TOKEN_LPAREN, "(" },
    { TOKEN_RPAREN, ")" },
    { TOKEN_LBRACE, "{" },
    { TOKEN_RBRACE, "}" },
    { TOKEN_LT,     "<" },
    { TOKEN_GT,     ">" }
};

struct Token
{
    Token() = default;

    std::string  token_value;
    TokenType    token_type;
};

} // namespace WFX::Core::Legacy

#endif