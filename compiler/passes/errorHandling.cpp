/*
 * Copyright 2004-2017 Cray Inc.
 * Other additional copyright holders may be indicated within.
 *
 * The entirety of this work is licensed under the Apache License,
 * Version 2.0 (the "License"); you may not use this file except
 * in compliance with the License.
 *
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "errorHandling.h"

#include "AstVisitorTraverse.h"
#include "CatchStmt.h"
#include "driver.h"
#include "stmt.h"
#include "symbol.h"
#include "TryStmt.h"
#include "wellknown.h"

#include <stack>

/*
This is a pseudo-code example of what this pass is supposed to do with a
throwing function, transforming error handling constructs.

If using try in functions that do not propagate, a catch-all is required
(catch or catch err). This can be avoided by using try!

Note that errors are not yet automatically deallocated when the error
is consumed by a catch.

// given code
proc propagate() throws {
  try {
    a(); // throws
    b(); // does not throw
    c(); // throws
  } catch e: SubError {
    f();
  } catch e: AnotherSubError {
    g();
  }
}

// after this pass
proc propagate(out error_out: Error) {
  var error: Error;
  a(error);
  if error then
    goto handler;
  b();
  c(error);
  if error then
    goto handler;

  label handler:
  if error {
    var e = error: SubError;
    if _cast {
      f();
      delete e;
    } else {
      var e = error: AnotherSubError;
      if e {
        g();
        delete e;
      } else {
        // set and return
        error_out = error;
        goto epilogue_label;
      }
    }
  }
}

nested try:
try {
  try {
    throwingCall();
  } catch e: SpecificError {
    handleGracefully();
  }
  otherThrowingCall();
} catch {
  handleSomehow();
}

{
  var _e1: Error;
  {
    var _e2: Error;
    throwingCall(_e2);
    if _e2 then
      goto handler2;

    label handler2:
    if _e2 {
      var _cast = _e2: SpecificError;
      if _cast {
        handleGracefully();
        delete _cast;
      } else {
        _e1 = _e2;
        goto handler1;
      }
    }
  }
  otherThrowingCall(_e1);
  if _e1 then
    goto handler1;

  label handler1:
  if _e1 {
    handleSomehow();
    delete _e1;
  }
}
*/


// This is the type for a map that is used when printing out the reason
// that a function is marked throws. Knowing the reason a function
// is marked throws implicitly is relevant for example with a coforall
// task function. In that case, the location of coforall itself
// isn't as relevant to the user as the location in the body
// of the coforall of a call to a throwing function.
//
// Here the key is a FnSymbol and the value is the reason it was
// marked throwing (typically a CallExpr).
typedef std::map<FnSymbol*, BaseAST*> implicitThrowsReasons_t;

// Static functions
static void markImplicitThrows(FnSymbol* fn, std::set<FnSymbol*>* visited, implicitThrowsReasons_t* reasons);
static void checkErrorHandling(FnSymbol* fn, implicitThrowsReasons_t * reasons);
static bool isCompilerGeneratedFunction(FnSymbol* fn);
static bool isUncheckedThrowsFunction(FnSymbol* fn);

namespace {


// Static class helper functions
static bool catchesNotExhaustive(TryStmt* tryStmt);
static bool shouldEnforceStrict(CallExpr* node);
static Expr* castToError(Symbol* errorExpr);

class ErrorHandlingVisitor : public AstVisitorTraverse {

public:
  ErrorHandlingVisitor       (ArgSymbol* _outFormal, LabelSymbol* _epilogue);

  virtual bool enterTryStmt (TryStmt*   node);
  virtual void exitTryStmt  (TryStmt*   node);
  virtual void exitCatchStmt(CatchStmt* node);
  virtual bool enterCallExpr(CallExpr*  node);

private:
  struct TryInfo {
    VarSymbol*   errorVar;
    LabelSymbol* handlerLabel;
    TryStmt*     tryStmt;
    BlockStmt*   tryBody;
  };

  std::stack<TryInfo> tryStack;
  std::stack<TryInfo> catchesStack;
  ArgSymbol*          outError;
  LabelSymbol*        epilogue;

  void   lowerCatches      (const TryInfo& info);
  AList  setOutGotoEpilogue(VarSymbol*     error);
  AList  errorCond         (VarSymbol*     errorVar,
                            BlockStmt*     thenBlock,
                            BlockStmt*     elseBlock = NULL);
  CallExpr* haltExpr       (VarSymbol*     error);

  ErrorHandlingVisitor();
};

ErrorHandlingVisitor::ErrorHandlingVisitor(ArgSymbol*   _outError,
                                           LabelSymbol* _epilogue) {
  outError = _outError;
  epilogue = _epilogue;
}

bool ErrorHandlingVisitor::enterTryStmt(TryStmt* node) {
  SET_LINENO(node);

  VarSymbol*   errorVar     = newTemp("error", dtError);
  errorVar->addFlag(FLAG_ERROR_VARIABLE);
  LabelSymbol* handlerLabel = new LabelSymbol("handler");
  handlerLabel->addFlag(FLAG_ERROR_LABEL);
  TryInfo      info         = {errorVar, handlerLabel, node, node->body()};
  tryStack.push(info);

  return true;
}

void ErrorHandlingVisitor::exitTryStmt(TryStmt* node) {
  SET_LINENO(node);

  TryInfo info = tryStack.top();
  tryStack.pop();

  BlockStmt* tryBody = info.tryBody;

  tryBody->insertAtHead(new CallExpr(PRIM_MOVE, info.errorVar, gNil));
  tryBody->insertAtHead(new DefExpr(info.errorVar));
  tryBody->insertAtTail(new DefExpr(info.handlerLabel));

  if (node->_catches.empty()) {
    lowerCatches(info); // no exitCatchStmt, so called here
  } else {
    catchesStack.push(info);
  }

  // may be NULL due to replacement of an enclosing try
  if (tryBody->parentExpr)
    tryBody->remove();

  node->replace(tryBody);
}

void ErrorHandlingVisitor::exitCatchStmt(CatchStmt* node) {
  // last CatchStmt to have its contents lowered; lower catches structure
  if (node->next == NULL) {
    TryInfo info = catchesStack.top();
    catchesStack.pop();
    lowerCatches(info);
  }
}

void ErrorHandlingVisitor::lowerCatches(const TryInfo& info) {
  TryStmt*   tryStmt  = info.tryStmt;
  VarSymbol* errorVar = info.errorVar;

  SET_LINENO(tryStmt);

  BlockStmt* handlers    = new BlockStmt();
  BlockStmt* currHandler = handlers;
  bool       hasCatchAll = false;

  for_alist(c, tryStmt->_catches) {
    if (hasCatchAll)
      INT_FATAL(c->prev, "catchall placed before the end of a catch list");

    SET_LINENO(c);

    CatchStmt* catchStmt = toCatchStmt(c);
    BlockStmt* catchBody = catchStmt->body();
    DefExpr*   catchDef  = catchStmt->expr();

    catchBody->insertAtTail(new CallExpr(gChplDeleteError, errorVar));
    catchBody->remove();

    // catchall
    if (catchDef == NULL) {
      hasCatchAll = true;
      currHandler->insertAtTail(catchBody);
    } else {
      VarSymbol* errSym  = toVarSymbol(catchDef->sym);
      Type*      errType = errSym->type;

      catchDef->remove();
      currHandler->insertAtTail(catchDef);

      // named catchall
      if (errType == dtError) {
        hasCatchAll = true;
        currHandler->insertAtTail(new CallExpr(PRIM_MOVE, errSym, errorVar));
        currHandler->insertAtTail(errorCond(errSym, catchBody));

      // specified catch
      } else {
        CallExpr*  castError   = new CallExpr(PRIM_DYNAMIC_CAST,
                                              new SymExpr(errType->symbol),
                                              errorVar);
        BlockStmt* nextHandler = new BlockStmt();

        currHandler->insertAtTail(new CallExpr(PRIM_MOVE, errSym, castError));
        currHandler->insertAtTail(errorCond(errSym, catchBody, nextHandler));

        currHandler = nextHandler;
      }
    }
  }

  if (!hasCatchAll) {
    if (tryStmt->tryBang()) {
      currHandler->insertAtTail(haltExpr(errorVar));
    } else if (!tryStack.empty()) {
      TryInfo* outerTry = & tryStack.top();
      currHandler->insertAtTail(new CallExpr(PRIM_MOVE, outerTry->errorVar,
                                             errorVar));
      currHandler->insertAtTail(new GotoStmt(GOTO_ERROR_HANDLING,
                                             outerTry->handlerLabel));
    } else if (outError != NULL) {
      currHandler->insertAtTail(setOutGotoEpilogue(errorVar));
    } else {
      INT_FATAL(tryStmt, "try without a catchall in a non-throwing function");
    }
  }

  info.tryBody->insertAtTail(errorCond(errorVar, handlers));
}

bool ErrorHandlingVisitor::enterCallExpr(CallExpr* node) {
  bool insideTry = !tryStack.empty();

  if (FnSymbol* calledFn = node->resolvedFunction()) {
    if (calledFn->throwsError()) {

      SET_LINENO(node);

      VarSymbol* errorVar    = NULL;
      BlockStmt* errorPolicy = new BlockStmt();
      Expr*      insert      = node->getStmtExpr();
      if (insert == NULL)
        insert = node;

      if (insideTry) {
        TryInfo info = tryStack.top();
        errorVar = info.errorVar;

        errorPolicy->insertAtTail(new GotoStmt(GOTO_ERROR_HANDLING,
                                               info.handlerLabel));
      } else {
        // without try, need an error variable
        errorVar = newTemp("error", dtError);
        errorVar->addFlag(FLAG_ERROR_VARIABLE);
        insert->insertBefore(new DefExpr(errorVar));
        insert->insertBefore(new CallExpr(PRIM_MOVE, errorVar, gNil));

        if (outError != NULL)
          errorPolicy->insertAtTail(setOutGotoEpilogue(errorVar));
        else
          errorPolicy->insertAtTail(haltExpr(errorVar));
      }

      node->insertAtTail(errorVar); // adding error argument to call

      // If we are calling a non-blocking task function,
      // we'll lower the error handling in parallel.cpp,
      // at the end of parallel(),  using lowerCheckErrorPrimitive.
      if (calledFn->hasFlag(FLAG_NON_BLOCKING) ||
          calledFn->hasFlag(FLAG_BEGIN) ||
          calledFn->hasFlag(FLAG_COBEGIN_OR_COFORALL)) {
        // Don't add errorPolicy block or condititonal.
      } else {
        // Regular operation
        insert->insertAfter(new CondStmt(new CallExpr(PRIM_CHECK_ERROR, errorVar), errorPolicy));
      }
    }
  } else if (node->isPrimitive(PRIM_THROW)) {
    SET_LINENO(node);

    BlockStmt* throwBlock = new BlockStmt();
    node->replace(throwBlock);

    SymExpr*   thrownExpr  = toSymExpr(node->get(1)->remove());
    VarSymbol* thrownError = toVarSymbol(thrownExpr->symbol());

    if (insideTry) {
      TryInfo   info      = tryStack.top();

      Expr* castError = castToError(thrownError);
      throwBlock->insertAtTail(new CallExpr(PRIM_MOVE, info.errorVar,
                                            castError));
      throwBlock->insertAtTail(new GotoStmt(GOTO_ERROR_HANDLING,
                                            info.handlerLabel));
    } else if (outError != NULL) {
      throwBlock->insertAtTail(setOutGotoEpilogue(thrownError));
    } else {
      INT_FATAL(node, "cannot throw in a non-throwing function");
    }
  }
  return true;
}

// Sets the fn out variable with the given error, then goes to the fn epilogue.
AList ErrorHandlingVisitor::setOutGotoEpilogue(VarSymbol* error) {

  Expr* castError = castToError(error);

  AList ret;
  // Using PRIM_ASSIGN instead of PRIM_MOVE here to work around
  // errors that come up in C compilation.
  ret.insertAtTail(new CallExpr(PRIM_ASSIGN, outError, castError));
  ret.insertAtTail(new GotoStmt(GOTO_RETURN, epilogue));

  return ret;
}

AList ErrorHandlingVisitor::errorCond(VarSymbol* errorVar,
                                      BlockStmt* thenBlock,
                                      BlockStmt* elseBlock) {
  VarSymbol* errorExistsVar = newTemp("shouldHandleError", dtBool);
  CallExpr*  errorExists    = new CallExpr(PRIM_CHECK_ERROR, errorVar);

  AList ret;
  ret.insertAtTail(new DefExpr(errorExistsVar));
  ret.insertAtTail(new CallExpr(PRIM_MOVE, errorExistsVar, errorExists));
  ret.insertAtTail(new CondStmt(new SymExpr(errorExistsVar),
                                thenBlock, elseBlock));
  return ret;
}

CallExpr* ErrorHandlingVisitor::haltExpr(VarSymbol* errorVar) {
  return new CallExpr(gChplUncaughtError, errorVar);
}

static void printReason(BaseAST* node, implicitThrowsReasons_t* reasons)
{
  if (reasons == NULL)
    return;

  if (CallExpr* call = toCallExpr(node)) {
    if (FnSymbol* calledFn = call->resolvedFunction()) {
      if (calledFn->throwsError()) {
        if (reasons->count(calledFn)) {
          BaseAST* reason = (*reasons)[calledFn];
          USR_PRINT(reason, " is reason function throws");
          printReason(reason, reasons);
        }
      }
    }
  }
}

// Returns true if the catches don't cover all of the cases.
static bool catchesNotExhaustive(TryStmt* tryStmt) {

  bool hasCatchAll = false;

  for_alist(c, tryStmt->_catches) {
    CatchStmt* catchStmt = toCatchStmt(c);
    DefExpr*   catchDef  = catchStmt->expr();

    // catchall
    if (catchDef == NULL) {
      hasCatchAll = true;
    } else {
      VarSymbol* errSym  = toVarSymbol(catchDef->sym);
      Type*      errType = errSym->type;

      // named catchall
      if (errType == dtError) {
        hasCatchAll = true;
      }
    }
  }

  return !hasCatchAll;
}

// Returns true if we should raise strict-mode errors
// for this call.
static bool shouldEnforceStrict(CallExpr* node) {
  if (FnSymbol* calledFn = node->resolvedFunction()) {
    bool inCompilerGeneratedFn = false;
    if (FnSymbol* parentFn = toFnSymbol(node->parentSymbol)) {
      // Don't check wrapper functions in strict mode.
      inCompilerGeneratedFn = isCompilerGeneratedFunction(parentFn);
    }
    bool callsUncheckedThrowsFn = isUncheckedThrowsFunction(calledFn);
    bool strictError = !(inCompilerGeneratedFn || callsUncheckedThrowsFn);

    return strictError;
  }
  return false;
}


static Expr* castToError(Symbol* error) {
  Expr* castError = NULL;

  if (error->type == dtError)
    castError = new SymExpr(error);
  else
    castError = new CallExpr(PRIM_CAST, dtError->symbol, error);

  return castError;
}

class ImplicitThrowsVisitor : public AstVisitorTraverse {

public:
  ImplicitThrowsVisitor(std::set<FnSymbol*>* visited, implicitThrowsReasons_t* reasons);

  virtual bool enterTryStmt  (TryStmt*   node);
  virtual void exitTryStmt   (TryStmt*   node);
  virtual bool enterCallExpr (CallExpr*  node);

  // Does this function throw?
  bool throws() { return canThrow; }
  // Is the visited function only throwing because of calls to functions
  // marked with FLAG_UNCHECKED_THROWS?
  bool unchecked() { return onlyUnchecked; }
  // What made us decide that this function should be marked throws?
  BaseAST* reason() { return reasonThrows; }

private:

  int  tryDepth;
  bool canThrow;
  bool onlyUnchecked;
  BaseAST* reasonThrows; // one of the reasons it throws, for errors
  std::set<FnSymbol*>* visited;
  implicitThrowsReasons_t* reasons;
};

ImplicitThrowsVisitor::ImplicitThrowsVisitor(std::set<FnSymbol*>* visitedIn, implicitThrowsReasons_t* reasonsIn) {
  tryDepth = 0;
  canThrow = false;
  onlyUnchecked = true;
  reasonThrows = NULL;
  visited = visitedIn;
  reasons = reasonsIn;
}

bool ImplicitThrowsVisitor::enterTryStmt(TryStmt* node) {
  tryDepth++;

  return true;
}

void ImplicitThrowsVisitor::exitTryStmt(TryStmt* node) {
  tryDepth--;

  // is it an exhaustive catch?

  bool nonExhaustive = catchesNotExhaustive(node);

  if (node->tryBang()) {
    canThrow = false;
  } else {
    canThrow = nonExhaustive;
    if (nonExhaustive)
      onlyUnchecked = false;
    if (reasonThrows == NULL)
      reasonThrows = node;
  }
}

bool ImplicitThrowsVisitor::enterCallExpr(CallExpr* node) {
  bool insideTry = (tryDepth > 0);

  if (FnSymbol* calledFn = node->resolvedFunction()) {

    // We might be calling a function that could be implicitly
    // throwing. For example, consider nested coforalls.
    // That will appear to be a call to coforall_fn1, and
    // that in turn appears to be a call to coforall_fn2.
    //
    // In that example, this enterCallExpr might be visiting
    // a call to coforall_fn2. We don't know yet if it throws
    // if we havn't visited it yet.
    markImplicitThrows(calledFn, visited, reasons);

    if (calledFn->throwsError()) {
      if (insideTry) {
        // OK
      } else {

        if (shouldEnforceStrict(node)) {
          if (reasonThrows == NULL)
            reasonThrows = node;
        }

        // not in a try
        canThrow = true;
        if (!calledFn->hasFlag(FLAG_UNCHECKED_THROWS))
          onlyUnchecked = false;
      }
    }
  } else if (node->isPrimitive(PRIM_THROW)) {
    canThrow = true;
    onlyUnchecked = false;
    if (reasonThrows == NULL)
      reasonThrows = node;
  }
  return true;
}


class ErrorCheckingVisitor : public AstVisitorTraverse {

public:
  ErrorCheckingVisitor(bool inThrowingFn, implicitThrowsReasons_t* reasons);

  virtual bool enterTryStmt  (TryStmt*   node);
  virtual void exitTryStmt   (TryStmt*   node);
  virtual bool enterCallExpr (CallExpr*  node);

private:
  implicitThrowsReasons_t* reasons;

  int  tryDepth;
  bool fnCanThrow;

  void checkCatches(TryStmt* tryStmt);
};

ErrorCheckingVisitor::ErrorCheckingVisitor(bool inThrowingFn, implicitThrowsReasons_t* inReasons) {
  tryDepth = 0;
  fnCanThrow = inThrowingFn;
  reasons = inReasons;
}

bool ErrorCheckingVisitor::enterTryStmt(TryStmt* node) {
  tryDepth++;

  return true;
}

void ErrorCheckingVisitor::exitTryStmt(TryStmt* node) {
  tryDepth--;

  checkCatches(node);

  // is it an exhaustive catch?
  bool nonExhaustive = catchesNotExhaustive(node);

  if (node->tryBang()) {
    // OK
  } else {
    if (tryDepth==0 && nonExhaustive && !fnCanThrow) {
      USR_FATAL_CONT(node, "try without a catchall in a non-throwing function");
    }
  }
}

void ErrorCheckingVisitor::checkCatches(TryStmt* tryStmt) {

  bool hasCatchAll = false;

  for_alist(c, tryStmt->_catches) {
    if (hasCatchAll)
      USR_FATAL_CONT(c->prev, "catchall placed before the end of a catch list");

    CatchStmt* catchStmt = toCatchStmt(c);
    DefExpr*   catchDef  = catchStmt->expr();

    // catchall
    if (catchDef == NULL) {
      hasCatchAll = true;
    } else {
      VarSymbol* errSym  = toVarSymbol(catchDef->sym);
      Type*      errType = errSym->type;

      // named catchall
      if (errType == dtError) {
        hasCatchAll = true;
      }
    }
  }
}

bool ErrorCheckingVisitor::enterCallExpr(CallExpr* node) {
  bool insideTry = (tryDepth > 0);

  if (FnSymbol* calledFn = node->resolvedFunction()) {
    if (calledFn->throwsError()) {
      if (insideTry) {
        // OK
      } else {
        if (shouldEnforceStrict(node)) {
          if (fStrictErrorHandling) {
            USR_FATAL_CONT(node, "throwing call without try or try! (strict mode)");
            printReason(node, reasons);
          }
        }
      }
    }
  } else if (node->isPrimitive(PRIM_THROW)) {
    if (insideTry) {
      // OK, error checking for this case done in try handling
    } else if (fnCanThrow == true) {
      // OK, fn can throw
    } else {
      USR_FATAL_CONT(node, "cannot throw in a non-throwing function");
    }
  }
  return true;
}

} /* end anon namespace */


// Returns `true` if a block can exit with an error
//  (e.g. by calling 'throw' or a throwing function,
//   when these are not handled by try! or catch).
// This function is useful to infer 'throws' for
// certain compiler-introduced functions.

static void markImplicitThrows(FnSymbol* fn, std::set<FnSymbol*>* visited, implicitThrowsReasons_t* reasons)
{
  // Currently, only task functions or iterators can be implicitly throws.
  if (!isTaskFun(fn) && !fn->isIterator())
    return;

  // If we already visited this function, don't visit it again.
  if (visited->count(fn) > 0)
    return;

  // Add to visited set first thing to prevent infinite recursion
  // if there ever is a cycle of calls.
  visited->insert(fn);

  ImplicitThrowsVisitor visit(visited, reasons);

  // Note that this function can recurse because
  // the ImplicitThrowsVisitor will call markImplicitThrows.
  fn->body->accept(&visit);

  if (visit.throws()) {
    // Update the reasons - for printing out error messages with
    // the user view of no task functions (say)
    (*reasons)[fn] = visit.reason();
    fn->throwsErrorInit();

    // If we only are throwing because of calls to functions marked
    // with FLAG_UNCHECKED_THROWS, this function should also be
    // so marked.
    if (visit.unchecked())
      fn->addFlag(FLAG_UNCHECKED_THROWS);
  }
}

static void checkErrorHandling(FnSymbol* fn, implicitThrowsReasons_t* reasons)
{
  ErrorCheckingVisitor visit(fn->throwsError(), reasons);

  fn->body->accept(&visit);
}

static void lowerErrorHandling(FnSymbol* fn)
{
  ArgSymbol*   outError = NULL;
  LabelSymbol* epilogue = NULL;

  if (fn->throwsError()) {
    SET_LINENO(fn);

    outError = new ArgSymbol(INTENT_REF, "error_out", dtError);
    outError->addFlag(FLAG_ERROR_VARIABLE);
    fn->insertFormalAtTail(outError);

    epilogue = fn->getOrCreateEpilogueLabel();
    INT_ASSERT(epilogue); // throws requires an epilogue
  }

  ErrorHandlingVisitor visitor = ErrorHandlingVisitor(outError, epilogue);
  fn->accept(&visitor);
}

void lowerCheckErrorPrimitive()
{
  forv_Vec(CallExpr, call, gCallExprs) {
    if (call->isPrimitive(PRIM_CHECK_ERROR)) {
      SET_LINENO(call);

      SymExpr* errSe   = toSymExpr(call->get(1));
      Symbol*  errorVar= errSe->symbol();

      VarSymbol* errorExistsVar = newTemp("errorExists", dtBool);
      DefExpr*   def            = new DefExpr(errorExistsVar);
      CallExpr*  errorExists    = new CallExpr(PRIM_NOTEQUAL, errorVar, gNil);
      CallExpr*  move = new CallExpr(PRIM_MOVE, errorExistsVar, errorExists);

      Expr* stmt = call->getStmtExpr();
      stmt->insertBefore(def);
      def->insertAfter(move);
      call->replace(new SymExpr(errorExistsVar));
    }
  }
}

bool isCheckErrorStmt(Expr* e)
{
  if (CondStmt* cond = toCondStmt(e)) {
    if (CallExpr* call = toCallExpr(cond->condExpr)) {
      if (call->isPrimitive(PRIM_CHECK_ERROR)) {
        return true;
      }
    }
  }
  return false;
}

// Should we raise an error in strict mode if the error is not handled?
// No for calls inside of compiler-generated functions, wrapper functions,
// or task functions. No for functions marked with FLAG_UNCHECKED_THROWS.
static bool isCompilerGeneratedFunction(FnSymbol* fn)
{
  return isTaskFun(fn) ||
         fn->hasFlag(FLAG_WRAPPER) ||
         fn->hasFlag(FLAG_COMPILER_GENERATED);
}

static bool isUncheckedThrowsFunction(FnSymbol* fn)
{
  return fn->hasFlag(FLAG_UNCHECKED_THROWS);
}


void lowerErrorHandling() {
  if (!fMinimalModules)
    INT_ASSERT(dtError->inTree());

  std::set<FnSymbol*> visited;
  implicitThrowsReasons_t reasons;

  forv_Vec(FnSymbol, fn, gFnSymbols) {
    // Determine if compiler-generated fns should be marked 'throws'
    markImplicitThrows(fn, &visited, &reasons);
  }

  // Note, the above step can adjust the AST in a way
  // that impacts this step.
  forv_Vec(FnSymbol, fn, gFnSymbols) {
    // Otherwise, just check for error-handling errors.
    checkErrorHandling(fn, &reasons);
  }

  // Quit if fatal errors were encountered by checkErrorHandling above.
  USR_STOP();

  forv_Vec(FnSymbol, fn, gFnSymbols) {
    lowerErrorHandling(fn);
  }

  // Note, PRIM_CHECK_ERROR will be lowered when a later
  // pass calls lowerCheckErrorPrimitive().
}
