#include <sstream>
#include "ast.h"
#include "lexer.h"

namespace {

int infix_bp(Tok t) {
  switch (t) {
    case Tok::OrOr: return 10;
    case Tok::AndAnd: return 20;
    case Tok::EqEq: case Tok::NotEq: return 30;
    case Tok::Lt: case Tok::Le: case Tok::Gt: case Tok::Ge: return 40;
    case Tok::Plus: case Tok::Minus: return 50;
    case Tok::Star: case Tok::Slash: case Tok::Percent: return 60;
    default: return -1;
  }
}
constexpr int PREFIX_BP = 70;

BinK to_bin(Tok t) {
  switch (t) {
    case Tok::Plus: return BinK::Add;
    case Tok::Minus: return BinK::Sub;
    case Tok::Star: return BinK::Mul;
    case Tok::Slash: return BinK::Div;
    case Tok::Percent: return BinK::Mod;
    case Tok::Lt: return BinK::Lt;
    case Tok::Le: return BinK::Le;
    case Tok::Gt: return BinK::Gt;
    case Tok::Ge: return BinK::Ge;
    case Tok::EqEq: return BinK::Eq;
    default: return BinK::Ne;
  }
}

class Parser {
 public:
  explicit Parser(std::vector<Token> t) : toks(std::move(t)) {}

  Program program() {
    Program p;
    while (peek().kind != Tok::Eof) p.fns.push_back(fn());
    return p;
  }

 private:
  std::vector<Token> toks;
  size_t i = 0;

  const Token& peek(size_t o = 0) const { return toks[std::min(i + o, toks.size() - 1)]; }
  Token next() { return toks[i < toks.size() - 1 ? i++ : i]; }
  bool accept(Tok k) {
    if (peek().kind == k) { i++; return true; }
    return false;
  }
  Token expect(Tok k) {
    if (peek().kind != k)
      compile_error(peek().line, std::string("expected ") + tok_name(k) + ", got " + tok_name(peek().kind));
    return next();
  }

  FnDecl fn() {
    FnDecl f;
    f.line = expect(Tok::Fn).line;
    f.name = expect(Tok::Ident).text;
    expect(Tok::LParen);
    if (!accept(Tok::RParen)) {
      do f.params.push_back(expect(Tok::Ident).text);
      while (accept(Tok::Comma));
      expect(Tok::RParen);
    }
    f.body = block();
    return f;
  }

  std::vector<std::unique_ptr<Stmt>> block() {
    expect(Tok::LBrace);
    std::vector<std::unique_ptr<Stmt>> out;
    while (!accept(Tok::RBrace)) {
      if (peek().kind == Tok::Eof) compile_error(peek().line, "unterminated block");
      out.push_back(stmt());
    }
    return out;
  }

  std::unique_ptr<Stmt> mk(SK k, int line) {
    auto s = std::make_unique<Stmt>();
    s->kind = k;
    s->line = line;
    return s;
  }

  std::unique_ptr<Stmt> stmt() {
    int line = peek().line;
    switch (peek().kind) {
      case Tok::Let: {
        next();
        auto s = mk(SK::Let, line);
        s->name = expect(Tok::Ident).text;
        expect(Tok::Assign);
        s->e = expr(0);
        expect(Tok::Semi);
        return s;
      }
      case Tok::If: return if_stmt();
      case Tok::While: {
        next();
        auto s = mk(SK::While, line);
        s->e = expr(0);
        s->body = block();
        return s;
      }
      case Tok::Return: {
        next();
        auto s = mk(SK::Return, line);
        if (peek().kind != Tok::Semi) s->e = expr(0);
        expect(Tok::Semi);
        return s;
      }
      case Tok::LBrace: {
        auto s = mk(SK::Block, line);
        s->body = block();
        return s;
      }
      case Tok::Ident:
        if (peek(1).kind == Tok::Assign) {
          auto s = mk(SK::Assign, line);
          s->name = next().text;
          next();
          s->e = expr(0);
          expect(Tok::Semi);
          return s;
        }
        [[fallthrough]];
      default: {
        auto s = mk(SK::ExprS, line);
        s->e = expr(0);
        expect(Tok::Semi);
        return s;
      }
    }
  }

  std::unique_ptr<Stmt> if_stmt() {
    auto s = mk(SK::If, expect(Tok::If).line);
    s->e = expr(0);
    s->body = block();
    if (accept(Tok::Else)) {
      if (peek().kind == Tok::If) s->els.push_back(if_stmt());
      else s->els = block();
    }
    return s;
  }

  std::unique_ptr<Expr> mke(EK k, int line) {
    auto e = std::make_unique<Expr>();
    e->kind = k;
    e->line = line;
    return e;
  }

  std::unique_ptr<Expr> prefix() {
    Token t = next();
    switch (t.kind) {
      case Tok::Int: { auto e = mke(EK::Int, t.line); e->ival = t.ival; return e; }
      case Tok::True: { auto e = mke(EK::Int, t.line); e->ival = 1; return e; }
      case Tok::False: { auto e = mke(EK::Int, t.line); e->ival = 0; return e; }
      case Tok::Nil: return mke(EK::Nil, t.line);
      case Tok::Ident: {
        if (accept(Tok::LParen)) {
          auto e = mke(EK::Call, t.line);
          e->name = t.text;
          if (!accept(Tok::RParen)) {
            do e->kids.push_back(expr(0));
            while (accept(Tok::Comma));
            expect(Tok::RParen);
          }
          return e;
        }
        auto e = mke(EK::Var, t.line);
        e->name = t.text;
        return e;
      }
      case Tok::LParen: {
        auto e = expr(0);
        expect(Tok::RParen);
        return e;
      }
      case Tok::Minus: case Tok::Bang: {
        auto e = mke(EK::Unary, t.line);
        e->uop = t.kind == Tok::Minus ? '-' : '!';
        e->kids.push_back(expr(PREFIX_BP));
        return e;
      }
      default:
        compile_error(t.line, std::string("expected an expression, got ") + tok_name(t.kind));
    }
  }

  std::unique_ptr<Expr> expr(int min_bp) {
    auto lhs = prefix();
    for (;;) {
      Tok op = peek().kind;
      int bp = infix_bp(op);
      if (bp <= min_bp) break;
      int line = next().line;
      auto rhs = expr(bp);
      EK k = op == Tok::AndAnd ? EK::And : op == Tok::OrOr ? EK::Or : EK::Binary;
      auto e = mke(k, line);
      e->bop = to_bin(op);
      e->kids.push_back(std::move(lhs));
      e->kids.push_back(std::move(rhs));
      lhs = std::move(e);
    }
    return lhs;
  }
};

const char* bin_name(BinK b) {
  static const char* n[] = {"+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!="};
  return n[(int)b];
}

void dump_expr(std::ostringstream& o, const Expr& e) {
  switch (e.kind) {
    case EK::Int: o << e.ival; break;
    case EK::Nil: o << "nil"; break;
    case EK::Var: o << e.name; break;
    case EK::Unary: o << "(" << e.uop << " "; dump_expr(o, *e.kids[0]); o << ")"; break;
    case EK::Binary: case EK::And: case EK::Or:
      o << "(" << (e.kind == EK::And ? "&&" : e.kind == EK::Or ? "||" : bin_name(e.bop)) << " ";
      dump_expr(o, *e.kids[0]);
      o << " ";
      dump_expr(o, *e.kids[1]);
      o << ")";
      break;
    case EK::Call:
      o << "(call " << e.name;
      for (auto& k : e.kids) { o << " "; dump_expr(o, *k); }
      o << ")";
      break;
  }
}

void dump_stmts(std::ostringstream& o, const std::vector<std::unique_ptr<Stmt>>& ss, int ind);

void dump_stmt(std::ostringstream& o, const Stmt& s, int ind) {
  std::string pad(ind * 2, ' ');
  o << pad;
  switch (s.kind) {
    case SK::Let: o << "(let " << s.name << " "; dump_expr(o, *s.e); o << ")\n"; break;
    case SK::Assign: o << "(set " << s.name << " "; dump_expr(o, *s.e); o << ")\n"; break;
    case SK::Return: o << "(return"; if (s.e) { o << " "; dump_expr(o, *s.e); } o << ")\n"; break;
    case SK::ExprS: dump_expr(o, *s.e); o << "\n"; break;
    case SK::Block: o << "(block\n"; dump_stmts(o, s.body, ind + 1); o << pad << ")\n"; break;
    case SK::While:
      o << "(while "; dump_expr(o, *s.e); o << "\n";
      dump_stmts(o, s.body, ind + 1);
      o << pad << ")\n";
      break;
    case SK::If:
      o << "(if "; dump_expr(o, *s.e); o << "\n";
      dump_stmts(o, s.body, ind + 1);
      if (!s.els.empty()) { o << pad << " else\n"; dump_stmts(o, s.els, ind + 1); }
      o << pad << ")\n";
      break;
  }
}

void dump_stmts(std::ostringstream& o, const std::vector<std::unique_ptr<Stmt>>& ss, int ind) {
  for (auto& s : ss) dump_stmt(o, *s, ind);
}

}

Program parse_program(const std::string& src) { return Parser(lex(src)).program(); }

std::string dump_ast_text(const Program& p) {
  std::ostringstream o;
  for (auto& f : p.fns) {
    o << "(fn " << f.name << " (";
    for (size_t i = 0; i < f.params.size(); i++) o << (i ? " " : "") << f.params[i];
    o << ")\n";
    dump_stmts(o, f.body, 1);
    o << ")\n";
  }
  return o.str();
}
