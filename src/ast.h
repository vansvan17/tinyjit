#pragma once
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

enum class EK { Int, Nil, Var, Unary, Binary, Call, And, Or };
enum class BinK { Add, Sub, Mul, Div, Mod, Lt, Le, Gt, Ge, Eq, Ne };

struct Expr {
  EK kind;
  int line = 0;
  int64_t ival = 0;
  std::string name;
  char uop = 0;
  BinK bop = BinK::Add;
  std::vector<std::unique_ptr<Expr>> kids;
};

enum class SK { Let, Assign, If, While, Return, ExprS, Block };

struct Stmt {
  SK kind;
  int line = 0;
  std::string name;
  std::unique_ptr<Expr> e;
  std::vector<std::unique_ptr<Stmt>> body;
  std::vector<std::unique_ptr<Stmt>> els;
};

struct FnDecl {
  std::string name;
  std::vector<std::string> params;
  std::vector<std::unique_ptr<Stmt>> body;
  int line = 0;
};

struct Program {
  std::vector<FnDecl> fns;
};

Program parse_program(const std::string& src);
std::string dump_ast_text(const Program& p);
