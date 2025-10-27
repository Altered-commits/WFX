/*
 * Copied this shit from my old project :)
 * Absolute garbage of a code and i'm not even going to change it. Its perfect
 * Here: https://github.com/Altered-commits/Flux-Bytecode-Interpreter
 */

#ifndef UNNAMED_LEXER_HPP
#define UNNAMED_LEXER_HPP

#include <string>
#include <cstdint>
#include <unordered_map>
#include "token.hpp"

#include "utils/logger/logger.hpp"

#define SANITY_CHECK(cnd) ((cur_chr != '\0') && (cnd))
#define IS_DIGIT(char) (char >= '0' && char <= '9')

#define IS_LOWER_CASE(char) (char >= 'a' && char <= 'z')
#define IS_UPPER_CASE(char) (char >= 'A' && char <= 'Z')
#define IS_CHAR(char) (IS_LOWER_CASE(char) || IS_UPPER_CASE(char))

#define IS_IDENT(char) (char == '_' || IS_DIGIT(char) || IS_CHAR(char))

namespace WFX::Core::Legacy {

class Lexer
{
    public:
        Lexer(std::string&& text_)
            : text(std::move(text_)), text_length(text.length()), cur_pos(0), cur_chr(text[cur_pos])
        {}

        Token& get_token();
        Token& get_current_token();
        Token  peek_next_token();
        static std::pair<std::size_t, std::size_t> getLineColCount();

    private:
        void advance();
        char peek(std::uint8_t);
        void set_token(std::string&&, TokenType);

        void skip_spaces();
        void skip_single_line_comments();
        void skip_multi_line_comments();
        void lex_digits();
        void lex_identifier_or_keyword();
        void lex_string_literal();
        void lex_this_or_eq_variation(const char*, const char*, TokenType, TokenType);

        void lex();

    private:
        std::string   text;
        std::uint64_t text_length;
    
    private:
        std::uint64_t cur_pos;
        char          cur_chr;
        Token         token;
    
    private:
        static std::size_t col;
        static std::size_t line;
    
    private:
        WFX::Utils::Logger& logger_ = WFX::Utils::Logger::GetInstance();

        //For Differentiation of keyword and identifier
        const std::unordered_map<std::string, TokenType> identifier_map = {
            {"Auto",     TOKEN_KEYWORD_AUTO},
            {"Void",     TOKEN_KEYWORD_VOID},
            {"Int",      TOKEN_KEYWORD_INT},
            {"Float",    TOKEN_KEYWORD_FLOAT},
            {"Cast",     TOKEN_KEYWORD_CAST},
            {"If",       TOKEN_KEYWORD_IF},
            {"Elif",     TOKEN_KEYWORD_ELIF},
            {"Else",     TOKEN_KEYWORD_ELSE},
            {"For",      TOKEN_KEYWORD_FOR},
            {"in",       TOKEN_KEYWORD_IN},
            {"is",       TOKEN_KEYWORD_IS},
            {"While",    TOKEN_KEYWORD_WHILE},
            {"Func",     TOKEN_KEYWORD_FUNC},
            {"Continue", TOKEN_KEYWORD_CONTINUE},
            {"Break",    TOKEN_KEYWORD_BREAK},
            {"Return",   TOKEN_KEYWORD_RETURN},
        };
};

} // namespace WFX::Core::Legacy

#endif