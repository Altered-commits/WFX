/* Check lexer.hpp first */
/*
 WELCOME TO THE SLOWEST LEXER WHICH I EVER BUILT, THIS IS SO BAD I WANNA DIE,
 WILL OPTIMIZE LATER CUZ THIS GIVES ME CANCER.
*/
#include <iostream>
#include "lexer.hpp"

namespace WFX::Core::Legacy {

//Global stuff ig
std::size_t Lexer::line = 1;
std::size_t Lexer::col = 1;

void Lexer::advance()
{
    if(cur_chr == '\n') {
        ++line; col = 0;
    }
    else
        ++col;
    cur_chr = text[++cur_pos];
}

char Lexer::peek(std::uint8_t offset)
{
    return (offset + cur_pos < text_length) ? text[offset + cur_pos] : text[text_length - 1];
}

void Lexer::skip_spaces()
{
    while (SANITY_CHECK(cur_chr == ' ' || cur_chr == '\t' || cur_chr == '\n'))
        advance();
}

void Lexer::skip_single_line_comments()
{
    while (SANITY_CHECK(cur_chr != '\n'))
        advance();
    //skip the newline as well
    advance();
}
/**/
void Lexer::skip_multi_line_comments()
{
    while (SANITY_CHECK(!(cur_chr == '*' && peek(1) == '/')))
        advance();

    //skip '*' and '/'
    advance();
    advance();
}

void Lexer::lex_digits()
{
    char* start_pos = &text[cur_pos];
    
    //Look only for digits -> 0..9
    while (SANITY_CHECK(IS_DIGIT(cur_chr)))
        advance();
    
    //When while loop breaks, it either hit '.' or some random character
    //If its not '.', its an integer return it
    if(cur_chr != '.')
    {
        set_token(std::string(start_pos, &text[cur_pos]), TOKEN_INT);
        return;
    }
    //But if it is a '.', make sure before lexing float, character after '.' is also not a dot
    if(peek(1) != '.')
    {    
        //Otherwise, we are expecting a floating type -> 123.123, lex more digits
        advance(); //Move past '.' as we are constructing string by pointers

        while(SANITY_CHECK(IS_DIGIT(cur_chr)))
            advance();
        
        //Set token as float
        set_token(std::string(start_pos, &text[cur_pos]), TOKEN_FLOAT);
        return;
    }
    //Else its '..' or some other character, return the current token as integer again
    set_token(std::string(start_pos, &text[cur_pos]), TOKEN_INT);
}

void Lexer::lex_identifier_or_keyword()
{
    char* start_pos = &text[cur_pos];

    while(SANITY_CHECK(IS_IDENT(cur_chr)))
        advance();
    
    //Construct string from starting to current character
    std::string temp(start_pos, &text[cur_pos]);

    //Using the identifier map, if the string exists in the map, its a keyword, else its just identifier
    auto elem = identifier_map.find(temp);
    set_token(std::move(temp), (elem != identifier_map.end()) ? elem->second : TOKEN_ID);
}

void Lexer::lex_string_literal()
{
    // WFX:
    std::string value;

    // Skip the opening "
    advance();

    while(true) 
    {
        if(cur_chr == '\0')
        {
            logger_.Fatal("[LegacyCode].[LexerError]: Unterminated string literal: ", value);
            return;
        }

        // End of string
        if(cur_chr == '"')
            break;

        // Handle escape sequence
        if(cur_chr == '\\')
        {
            // Move to the char after '\'
            advance();
            
            switch(cur_chr)
            {
                case 'n':  value.push_back('\n'); break;
                case 't':  value.push_back('\t'); break;
                case 'r':  value.push_back('\r'); break;
                case '"':  value.push_back('"');  break; // Escaped quote
                case '\\': value.push_back('\\'); break; // Escaped backslash
                case '\0': // Escape at end of file
                    logger_.Fatal("[LegacyCode].[LexerError]: Unterminated string literal (ends with escape)");
                    return;
                default:
                    // Just append the character as is (e.g., \a becomes 'a')
                    value.push_back(cur_chr);
                    break;
            }
        }
        // Its a regular character
        else
            value.push_back(cur_chr);

        // Move to the next character
        advance();
    }

    // We are at the closing quote, so skip it
    advance(); 
    
    set_token(std::move(value), TOKEN_STRING);
}

void Lexer::lex_this_or_eq_variation(const char* text, const char* text_with_eq, TokenType type, TokenType type_with_eq)
{
    advance();
    if(cur_chr == '=')
    {
        advance();
        set_token(text_with_eq, type_with_eq);
        return;
    }
    set_token(text, type);
    return;
}

void Lexer::lex() 
{
    while(true)
    {
        skip_spaces();

        if(IS_DIGIT(cur_chr))
        {
            lex_digits();
            return;
        }
        if(IS_CHAR(cur_chr) || cur_chr == '_')
        {
            lex_identifier_or_keyword();
            return;
        }

        switch (cur_chr)
        {
            case '"':
                lex_string_literal();
                return;
            case '+':
                set_token("+", TOKEN_PLUS);
                advance();
                return;
            case '-':
                set_token("-", TOKEN_MINUS);
                advance();
                return;
            case '*':
                set_token("*", TOKEN_MULT);
                advance();
                return;
            //Check for comments as well
            case '/':
                advance();
                //Single line comments
                if(cur_chr == '/')
                {
                    skip_single_line_comments();
                    break;
                }
                //Multi line comments
                if(cur_chr == '*')
                {
                    skip_multi_line_comments();
                    break;
                }
                set_token("/", TOKEN_DIV);
                return;
            case '%':
                set_token("%", TOKEN_MODULO);
                advance();
                return;
            case '(':
                set_token("(", TOKEN_LPAREN);
                advance();
                return;
            case ')':
                set_token(")", TOKEN_RPAREN);
                advance();
                return;
            case '{':
                set_token("{", TOKEN_LBRACE);
                advance();
                return;
            case '}':
                set_token(")", TOKEN_RBRACE);
                advance();
                return;
            case '^':
                set_token("^", TOKEN_POW);
                advance();
                return;
            case '&':
                advance();
                if(cur_chr == '&') {
                    advance();
                    set_token("&&", TOKEN_AND);
                    return;
                }
                logger_.Fatal("[LegacyCode].[LexerError]: '&' bitwise operator currently not supported");
                return;
            case '|':
                advance();
                if(cur_chr == '|') {
                    advance();
                    set_token("||", TOKEN_OR);
                    return;
                }
                logger_.Fatal("[LegacyCode].[LexerError]: '|' bitwise operator currently not supported");
                return;
            //Functions self explanatory
            case '<':
                //Check either '<' or '<='
                lex_this_or_eq_variation("<", "<=", TOKEN_LT, TOKEN_LTEQ);
                return;
            case '>':
                //Check either '>' or '>='
                lex_this_or_eq_variation(">", ">=", TOKEN_GT, TOKEN_GTEQ);
                return;
            case '!':
                //Either '!' not operator, or '!=' operator
                lex_this_or_eq_variation("!", "!=", TOKEN_NOT, TOKEN_NEQ);
                return;
            case '=':
                //Check if its '==' or simple '='
                lex_this_or_eq_variation("=", "==", TOKEN_EQ, TOKEN_EEQ);
                return;
            //Ternary
            case '?':
                set_token("?", TOKEN_QUESTION);
                advance();
                return;
            case ':':
                set_token(":", TOKEN_COLON);
                advance();
                return;
            //,
            case ',':
                set_token(",", TOKEN_COMMA);
                advance();
                return;
            //. or .. or ...
            case '.':
                advance();
                if(cur_chr == '.') {
                    advance();
                    if(cur_chr == '.')
                    {
                        set_token("...", TOKEN_ELLIPSIS);
                        advance();
                        return;
                    }
                    set_token("..", TOKEN_RANGE);
                    return;
                }
                set_token(".", TOKEN_DOT);
                return;
            //End of statements
            case ';':
                set_token(";", TOKEN_SEMIC);
                advance();
                return;
            case '\0':
                set_token("EOF", TOKEN_EOF);
                return;
            
            default:
                logger_.Fatal("[LegacyCode].[LexerError]: Character not supported, Character: ", cur_chr);
        }
    }
}
//------------------------------------------
Token& Lexer::get_token()
{
    lex();
    return token;
}

Token& Lexer::get_current_token()
{
    return token;
}

Token Lexer::peek_next_token()
{
    //Save lexer state
    Token saved_token = token;
    std::uint64_t saved_pos = cur_pos;
    char saved_char = cur_chr;
    
    //Lex
    lex();
    Token next = token;
    
    //Rewind lexer state
    token = saved_token;
    cur_pos = saved_pos;
    cur_chr = saved_char;
    
    //Return
    return next;
}

void Lexer::set_token(std::string&& token_value, TokenType token_type)
{
    token.token_value = std::move(token_value);
    token.token_type  = token_type;
}

std::pair<std::size_t, std::size_t> Lexer::getLineColCount()
{
    return {line, col};
}

} // namespace WFX::Core::Legacy