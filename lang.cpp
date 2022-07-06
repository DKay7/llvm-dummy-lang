#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/IR/Verifier.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace llvm;


//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
//--------------------------------------------------------------------------------
// Lexer
//--------------------------------------------------------------------------------
//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

enum Tokens {
    TOK_EOF         = -1,

    TOK_DEF         = -2,
    TOK_EXTERN      = -3,

    TOK_IDENTIFIER  = -4,
    TOK_NUMBER      = -5
};

static std::string identifier_str;
static double num_val;

static int get_token() {
    static int last_char = ' ';

    while (isspace(last_char))
        last_char = getchar ();

    if (isalpha (last_char)) {
        identifier_str = last_char;
        
        while (isalnum (last_char = getchar ()))
            identifier_str += last_char;

        if (identifier_str == "def")
            return TOK_DEF;
        
        if (identifier_str == "extern")
            return TOK_EXTERN;
        
        return TOK_IDENTIFIER;
    }


    if (isdigit (last_char) || last_char == '.') {
        std::string number_str;
        int num_points = 0;

        do {
            last_char = getchar ();
            
            if (last_char == '.')
                num_points += 1;

        } while ((isdigit (last_char) || last_char == '.') && num_points <= 1);

        num_val = strtod (number_str.c_str(), nullptr);
        return TOK_NUMBER;
    }

    if (last_char == '#') {
        do { 
            last_char = getchar ();
        } while (last_char != EOF && last_char != '\n' && last_char != '\r');

        if (last_char != EOF)
            return get_token ();
    }
    
    if (last_char == EOF)
        return TOK_EOF;
    
    int curr_char = last_char;
    last_char = getchar ();

    return curr_char;
}


//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
//--------------------------------------------------------------------------------
// AST
//--------------------------------------------------------------------------------
//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

namespace {

class ExprAST {
    public:
        virtual ~ExprAST () = default;
        virtual Value *codegen() = 0;
};

class NumberExprAST: public ExprAST {
    double num_value;

    public:
        NumberExprAST (double num): num_value(num) {}
        Value *codegen() override;
};


class VariableExprAST: public ExprAST {
    std::string name;
    
    public:
        VariableExprAST (const std::string& name): name(name) {}
        Value *codegen() override;
};


class BinaryExprAST: public ExprAST {
    char op;
    std::unique_ptr<ExprAST> LHS, RHS;

    public:
        BinaryExprAST (char op, std::unique_ptr<ExprAST> LHS, std::unique_ptr<ExprAST> RHS)
            : op(op), LHS(std::move(LHS)), RHS(std::move(RHS)) {}
        Value *codegen() override;

};


class CallExprAST: public ExprAST {
    std::string name;
    std::vector<std::unique_ptr<ExprAST>> args;

    public:
        CallExprAST (const std::string& callee_name, std::vector<std::unique_ptr<ExprAST>> args)
            : name (callee_name), args (std::move(args)) {}
        Value *codegen() override;

};


class PrototypeAST {
    std::string name;
    std::vector<std::string> args;

    public:
        PrototypeAST (const std::string& name, std::vector<std::string> args)
            : name (name), args(std::move(args)) {}
        
        const std::string &get_name () const { return name; }
        Function *codegen();
};


class FunctionAST {
    std::unique_ptr<PrototypeAST>  prototype;
    std::unique_ptr<ExprAST>       body;

    public:
        FunctionAST (std::unique_ptr<PrototypeAST> prototype, std::unique_ptr<ExprAST> body)
            : prototype (std::move(prototype)), body (std::move(body)) {}
        Function *codegen();
};


}


//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
//--------------------------------------------------------------------------------
// PARSER
//--------------------------------------------------------------------------------
//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

static int current_token;
static std::map<char, int> binary_op_precedence;
static int get_next_token () { return current_token = get_token (); }

static int get_token_precedence () {
    if (!isascii (current_token))
        return -1;

    int token_precedence = binary_op_precedence[current_token];
    if (token_precedence <= 0)
        return -1;
    
    return token_precedence;
}

std::unique_ptr<ExprAST> log_error (const char* err_str) {
    fprintf (stderr, "Error: %s\n", err_str);
    return nullptr;
}

std::unique_ptr<PrototypeAST> log_error_p (const char* err_str) {
    log_error (err_str);
    return nullptr;
}

static std::unique_ptr<ExprAST> parse_expression ();

///numberexpr ::= number
static std::unique_ptr<ExprAST> parse_number_expression () {
    auto result = std::make_unique<NumberExprAST> (num_val);
    get_next_token (); // eat number
    return std::move(result);
}

///parenexpr ::= '(' expression ')'
static std::unique_ptr<ExprAST> parse_paren_expression () {
    get_next_token (); // eat (
    auto expr = parse_expression ();

    if (!expr)
        return nullptr;

    if (current_token != ')')
        return log_error ("expected ')'");

    get_next_token (); // eat )

    return expr;
}

/// idetifier 
///     ::= identifier
///     ::= identifier '(' expression ')'
static std::unique_ptr<ExprAST> parse_identifier_expression () {
    std::string identifier_name = identifier_str;

    get_next_token (); // eat identifier;

    if (current_token != '(') // then its simple var ref
        return std::make_unique<VariableExprAST> (identifier_name);
    
    // else it's call
    get_next_token (); // eat (
    std::vector<std::unique_ptr<ExprAST>> call_args;

    if (current_token != ')') {
        while (true) {
            if (auto arg = parse_expression ()) {
                call_args.push_back (std::move(arg));
            }

            if (current_token == ')')
                break;
            
            if (current_token != ',')
                return log_error ("expected ')' or ',' ");
            
            get_next_token();
        }
    }

    get_next_token (); // eat )

    return std::make_unique<CallExprAST>(identifier_name, std::move(call_args));
}

/// primary 
///     ::= identifier
///     ::= numberexpr 
///     ::= parenexpr
static std::unique_ptr<ExprAST> parse_primary() {
    switch (current_token) {
        case TOK_IDENTIFIER:
            return parse_identifier_expression ();
        case TOK_NUMBER:
            return parse_number_expression ();
        case '(':
            return parse_paren_expression ();
        default:
            return log_error ("unknown token");
    }
}

/// binoprhs
///     ::= ('+' primary)*
static std::unique_ptr<ExprAST> parse_bin_op_rhs (int expr_prec, std::unique_ptr<ExprAST> LHS) {
    while (true) {
        int token_prec = get_token_precedence ();

        if (token_prec < expr_prec)
            return LHS;
        
        int binop = current_token;
        get_next_token ();  // eat_binop

        auto RHS = parse_primary ();
        if (!RHS)
            return nullptr;
        
        int next_prec = get_token_precedence ();
        if (token_prec < next_prec) {
            RHS = parse_bin_op_rhs (token_prec + 1, std::move(RHS));
            if (!RHS)
                return nullptr; 
        }

        //merge LHS, RHS
        LHS = std::make_unique<BinaryExprAST>(binop, std::move(LHS), std::move(RHS));
    }

} 

/// expression ::= primary binoprhs
std::unique_ptr<ExprAST> parse_expression () {
    auto LHS = parse_primary ();
    if (!LHS)
        return nullptr;
    
    return parse_bin_op_rhs (0, std::move(LHS));
}

/// prototype ::= identifier '(' identifier* ')'
static std::unique_ptr<PrototypeAST> parse_prototype () {
    if (current_token != TOK_IDENTIFIER)
        return log_error_p ("Expected function name in prototype");

    std::string func_name = identifier_str;
    get_next_token ();

    if (current_token != '(')
        return log_error_p ("Expected '(' in prototype");

    std::vector<std::string> arg_names;
    while (get_next_token () == TOK_IDENTIFIER)
        arg_names.push_back (identifier_str);
    
    if (current_token != ')')
        return log_error_p ("Expected ')' in prototype");
    
    get_next_token (); // eat )

    return std::make_unique<PrototypeAST> (func_name, std::move(arg_names));
}

/// definition ::= 'def' prototype expression
static std::unique_ptr<FunctionAST> parse_definition () {
    get_next_token (); // eat def
    auto prototype = parse_prototype ();
    
    if (!prototype)
        return nullptr;
    
    if (auto expr = parse_expression ())
        return std::make_unique<FunctionAST> (std::move(prototype), std::move(expr));

    return nullptr;
    
}

/// toplevelexpr ::= expression
std::unique_ptr<FunctionAST> parse_toplevel_expression () {
    if (auto expression = parse_expression ()) {    
        auto prototype = std::make_unique<PrototypeAST>("__anon_expr", std::vector<std::string>()); 
        return std::make_unique<FunctionAST> (std::move(prototype), std::move(expression));
    }

    return nullptr;
}

/// extern ::= 'extern' prototype
std::unique_ptr<PrototypeAST> parse_extern () {
    get_next_token (); // eat 'extern'
    return parse_prototype ();
}


//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
//--------------------------------------------------------------------------------
// CODE GENERATION
//--------------------------------------------------------------------------------
//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

static std::unique_ptr<LLVMContext> the_context;
static std::unique_ptr<Module> the_module;
static std::unique_ptr<IRBuilder<>> builder;
static std::map<std::string, Value *> named_values;

Value *log_error_v(const char *err_string) {
  log_error(err_string);
  return nullptr;
}

Value *NumberExprAST::codegen() {
    return ConstantFP::get(*the_context, APFloat(num_value));
}

Value *VariableExprAST::codegen() {
  // Look this variable up in the function.
  Value *value = named_values[name];
  
  if (!value)
    return log_error_v("Unknown variable name");
  
  return value;
}

Value *BinaryExprAST::codegen() {
  Value *L = LHS->codegen();
  Value *R = RHS->codegen();
  if (!L || !R)
    return nullptr;

  switch (op) {
  case '+':
    return builder->CreateFAdd(L, R, "addtmp");
  case '-':
    return builder->CreateFSub(L, R, "subtmp");
  case '*':
    return builder->CreateFMul(L, R, "multmp");
  case '<':
    L = builder->CreateFCmpULT(L, R, "cmptmp");
    // Convert bool 0/1 to double 0.0 or 1.0
    return builder->CreateUIToFP(L, Type::getDoubleTy(*the_context), "booltmp");
  default:
    return log_error_v("invalid binary operator");
  }
}

Value *CallExprAST::codegen() {
  // Look up the name in the global module table.
  Function *callee_func = the_module->getFunction(name);
  if (!callee_func)
    return log_error_v("Unknown function referenced");

  // If argument mismatch error.
  if (callee_func->arg_size() != args.size())
    return log_error_v("Incorrect # arguments passed");

  std::vector<Value *> args_vec;
  for (unsigned i = 0, e = args.size(); i != e; ++i) {
    args_vec.push_back(args[i]->codegen());
    if (!args_vec.back())
      return nullptr;
  }

  return builder->CreateCall(callee_func, args_vec, "calltmp");
}

Function *PrototypeAST::codegen() {
  std::vector<Type *> doubles_vec(args.size(), Type::getDoubleTy(*the_context));
  FunctionType *func_type 
        = FunctionType::get(Type::getDoubleTy(*the_context), doubles_vec, false);

  Function *func 
        = Function::Create(func_type, Function::ExternalLinkage, name, the_module.get());

  // Set names for all arguments.
  unsigned Idx = 0;
  for (auto &arg : func->args())
    arg.setName(args[Idx++]);

  return func;
}

Function *FunctionAST::codegen() {
  // First, check for an existing function from a previous 'extern' declaration.
  Function *the_func = the_module->getFunction(prototype->get_name());

  if (!the_func)
    the_func = prototype->codegen();

  if (!the_func)
    return nullptr;

  // Create a new basic block to start insertion into.
  BasicBlock *basic_block = BasicBlock::Create(*the_context, "entry", the_func);
  builder->SetInsertPoint(basic_block);

  // Record the function arguments in the named_values map.
  named_values.clear();
  for (auto &arg : the_func->args())
    named_values[std::string(arg.getName())] = &arg;

  if (Value *ret_val = body->codegen()) {
    // Finish off the function.
    builder->CreateRet(ret_val);

    // Validate the generated code, checking for consistency.
    verifyFunction(*the_func);

    return the_func;
  }

  // Error reading body, remove function.
  the_func->eraseFromParent();
  return nullptr;
}


//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
//--------------------------------------------------------------------------------
// TOP-LEVEL PARSING
//--------------------------------------------------------------------------------
//flexxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx

static void initialize_module() {
  the_context = std::make_unique<LLVMContext>();
  the_module = std::make_unique<Module>("my cool jit", *the_context);

  builder = std::make_unique<IRBuilder<>>(*the_context);
}

static void handle_definition () {
    if (auto FnAST = parse_definition ()) {
        fprintf (stderr, "Parsed a func. definition\n");

        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Read function definition:");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    } else
        get_next_token ();
}

static void handle_extern  () {
    if (auto ProtoAST = parse_extern ()){
        fprintf (stderr, "Parsed an extern\n");

        if (auto *FnIR = ProtoAST->codegen()) {
            fprintf(stderr, "Read extern: ");
            FnIR->print(errs());
            fprintf(stderr, "\n");
        }
    }
    else
        get_next_token ();
}

static void handle_toplevel_expression () {
    if (auto FnAST = parse_toplevel_expression()){
        fprintf (stderr, "Parsed an top-level expression\n");
        if (auto *FnIR = FnAST->codegen()) {
            fprintf(stderr, "Read top-level expression:");
            FnIR->print(errs());
            fprintf(stderr, "\n");

            // Remove the anonymous expression.
            FnIR->eraseFromParent();
        }
    }
    else
        get_next_token ();
}

static void main_loop () {
    while (true) {
        fprintf (stderr, "input: ");
        switch (current_token) {
            case TOK_EOF:
                return;
            case ';':
                get_next_token ();
                break;
            case TOK_DEF:
                handle_definition ();
                break;
            case TOK_EXTERN:
                handle_extern ();
                break;
            default:
                handle_toplevel_expression ();
                break;
        }
    }
}

int main () { 
    binary_op_precedence['<'] = 10;
    binary_op_precedence['+'] = 20;
    binary_op_precedence['-'] = 20;
    binary_op_precedence['*'] = 40;

    fprintf (stderr, "input: ");
    get_next_token ();

    initialize_module ();

    main_loop ();

    the_module->print(errs(), nullptr);
    return 0;
}