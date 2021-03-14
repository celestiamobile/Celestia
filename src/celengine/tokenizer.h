// tokenizer.h
//
// Copyright (C) 2001-2009, the Celestia Development Team
// Original version by Chris Laurel <claurel@gmail.com>
//
// This program is free software; you can redistribute it and/or
// modify it under the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 2
// of the License, or (at your option) any later version.

#ifndef _TOKENIZER_H_
#define _TOKENIZER_H_

#include <string>
#include <iosfwd>

class Tokenizer
{
public:
    enum TokenType
    {
        TokenName           = 0,
        TokenString         = 1,
        TokenNumber         = 2,
        TokenBegin          = 3,
        TokenEnd            = 4,
        TokenNull           = 5,
        TokenBeginGroup     = 6,
        TokenEndGroup       = 7,
        TokenBeginArray     = 8,
        TokenEndArray       = 9,
        TokenEquals         = 10,
        TokenError          = 11,
        TokenBar            = 12,
        TokenBeginUnits     = 13,
        TokenEndUnits       = 14,
    };

    Tokenizer(std::istream*);

    TokenType nextToken();
    TokenType getTokenType();
    void pushBack();
    double getNumberValue();
    std::string getNameValue();
    std::string getStringValue();

    int getLineNumber() const;

private:
    enum State
    {
        StartState          = 0,
        NameState           = 1,
        NumberState         = 2,
        FractionState       = 3,
        ExponentState       = 4,
        ExponentFirstState  = 5,
        DotState            = 6,
        CommentState        = 7,
        StringState         = 8,
        ErrorState          = 9,
        StringEscapeState   = 10,
        UnicodeEscapeState  = 11,
    };

    std::istream* in;

    int nextChar { 0 };
    TokenType tokenType{ TokenBegin };
    bool haveValidNumber{ false };
    bool haveValidName{ false };
    bool haveValidString{ false };

    unsigned int unicodeValue{ 0 };
    unsigned int unicodeEscapeDigits{ 0 };

    bool pushedBack{ false };

    int readChar();
    void syntaxError(const char*);

    double numberValue{ 0.0 };

    std::string textToken;

    int lineNum{ 1 };
};

#endif // _TOKENIZER_H_
