#pragma once
#include <cstdint>
#include <string>
#include <vector>

enum class Tok {
  Eof, Ident, Int,
  Fn, Let, If, Else, While, Return, Nil, True, False,
  LParen, RParen, LBrace, RBrace, Comma, Semi, Assign,
  Plus, Minus, Star, Slash, Percent,
  Lt, Le, Gt, Ge, EqEq, NotEq, Bang, AndAnd, OrOr,
};

struct Token {
  Tok kind;
  std::string text;
  int64_t ival = 0;
  int line = 0;
};

std::vector<Token> lex(const std::string& src);
const char* tok_name(Tok t);

[[noreturn]] void compile_error(int line, const std::string& msg);
