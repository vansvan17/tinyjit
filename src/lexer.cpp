#include "lexer.h"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <unordered_map>

[[noreturn]] void compile_error(int line, const std::string& msg) {
  fprintf(stderr, "error: line %d: %s\n", line, msg.c_str());
  exit(2);
}

const char* tok_name(Tok t) {
  switch (t) {
    case Tok::Eof: return "end of file";
    case Tok::Ident: return "identifier";
    case Tok::Int: return "integer";
    case Tok::Fn: return "'fn'";
    case Tok::Let: return "'let'";
    case Tok::If: return "'if'";
    case Tok::Else: return "'else'";
    case Tok::While: return "'while'";
    case Tok::Return: return "'return'";
    case Tok::Nil: return "'nil'";
    case Tok::True: return "'true'";
    case Tok::False: return "'false'";
    case Tok::LParen: return "'('";
    case Tok::RParen: return "')'";
    case Tok::LBrace: return "'{'";
    case Tok::RBrace: return "'}'";
    case Tok::Comma: return "','";
    case Tok::Semi: return "';'";
    case Tok::Assign: return "'='";
    case Tok::Plus: return "'+'";
    case Tok::Minus: return "'-'";
    case Tok::Star: return "'*'";
    case Tok::Slash: return "'/'";
    case Tok::Percent: return "'%'";
    case Tok::Lt: return "'<'";
    case Tok::Le: return "'<='";
    case Tok::Gt: return "'>'";
    case Tok::Ge: return "'>='";
    case Tok::EqEq: return "'=='";
    case Tok::NotEq: return "'!='";
    case Tok::Bang: return "'!'";
    case Tok::AndAnd: return "'&&'";
    case Tok::OrOr: return "'||'";
  }
  return "?";
}

std::vector<Token> lex(const std::string& s) {
  static const std::unordered_map<std::string, Tok> kw = {
      {"fn", Tok::Fn},         {"let", Tok::Let},     {"if", Tok::If},
      {"else", Tok::Else},     {"while", Tok::While}, {"return", Tok::Return},
      {"nil", Tok::Nil},       {"true", Tok::True},   {"false", Tok::False},
  };
  std::vector<Token> out;
  size_t i = 0;
  int line = 1;
  auto push = [&](Tok k, std::string text) { out.push_back({k, std::move(text), 0, line}); };
  while (i < s.size()) {
    char c = s[i];
    if (c == '\n') { line++; i++; continue; }
    if (isspace((unsigned char)c)) { i++; continue; }
    if (c == '/' && i + 1 < s.size() && s[i + 1] == '/') {
      while (i < s.size() && s[i] != '\n') i++;
      continue;
    }
    if (isdigit((unsigned char)c)) {
      size_t j = i;
      unsigned __int128 v = 0;
      while (j < s.size() && isdigit((unsigned char)s[j])) {
        v = v * 10 + (s[j] - '0');
        if (v > ((unsigned __int128)1 << 62) - 1) compile_error(line, "integer literal too large (ints are 63-bit)");
        j++;
      }
      Token t{Tok::Int, s.substr(i, j - i), (int64_t)v, line};
      out.push_back(t);
      i = j;
      continue;
    }
    if (isalpha((unsigned char)c) || c == '_') {
      size_t j = i;
      while (j < s.size() && (isalnum((unsigned char)s[j]) || s[j] == '_')) j++;
      std::string w = s.substr(i, j - i);
      auto it = kw.find(w);
      push(it == kw.end() ? Tok::Ident : it->second, w);
      i = j;
      continue;
    }
    auto two = [&](char a, char b) { return c == a && i + 1 < s.size() && s[i + 1] == b; };
    if (two('<', '=')) { push(Tok::Le, "<="); i += 2; continue; }
    if (two('>', '=')) { push(Tok::Ge, ">="); i += 2; continue; }
    if (two('=', '=')) { push(Tok::EqEq, "=="); i += 2; continue; }
    if (two('!', '=')) { push(Tok::NotEq, "!="); i += 2; continue; }
    if (two('&', '&')) { push(Tok::AndAnd, "&&"); i += 2; continue; }
    if (two('|', '|')) { push(Tok::OrOr, "||"); i += 2; continue; }
    Tok k;
    switch (c) {
      case '(': k = Tok::LParen; break;
      case ')': k = Tok::RParen; break;
      case '{': k = Tok::LBrace; break;
      case '}': k = Tok::RBrace; break;
      case ',': k = Tok::Comma; break;
      case ';': k = Tok::Semi; break;
      case '=': k = Tok::Assign; break;
      case '+': k = Tok::Plus; break;
      case '-': k = Tok::Minus; break;
      case '*': k = Tok::Star; break;
      case '/': k = Tok::Slash; break;
      case '%': k = Tok::Percent; break;
      case '<': k = Tok::Lt; break;
      case '>': k = Tok::Gt; break;
      case '!': k = Tok::Bang; break;
      default: compile_error(line, std::string("unexpected character '") + c + "'");
    }
    push(k, std::string(1, c));
    i++;
  }
  push(Tok::Eof, "");
  return out;
}
