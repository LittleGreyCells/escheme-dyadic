#include "eval.hxx"

#include <cstdlib>

#include "code.hxx"

#include "core/error.hxx"
#include "core/symtab.hxx"
#include "core/memory.hxx"
#include "core/printer.hxx"

namespace escheme
{

using MEMORY::cons;
using MEMORY::fixnum;

// evaluator registers
SEXPR EVAL::exp;
SEXPR EVAL::env;
SEXPR EVAL::val;
SEXPR EVAL::aux;
SEXPR EVAL::unev;
EVSTATE EVAL::cont;
EVSTATE EVAL::next;
SEXPR EVAL::theGlobalEnv;

int EVAL::pc;

SEXPR EVAL::map_code;
SEXPR EVAL::for_code;
SEXPR EVAL::rte_code;
SEXPR EVAL::rtc_code;
SEXPR EVAL::fep_code;

//
// New: A frame-based representation
//
//   <env> = ( <frame> . <env> )
//
// The following functions are dependent upon the representation:
//
//   lookup
//   set_variable_value
//   create_bindings
//   extend_environment
//

SEXPR EVAL::lookup( SEXPR var, SEXPR env )
{
   for (; anyp(env); env = getenvbase(env))
   {
      FRAME frame = getenvframe(env);
      SEXPR vars = getframevars(frame);
      
      for ( int i = 0; anyp(vars); ++i, vars = getcdr(vars) )
      {
         if (getcar(vars) == var) 
            return frameref(frame, i);
      }
   }

   // global var
   const SEXPR val = value(var);

   if (val == symbol_unbound)
      ERROR::severe("symbol is unbound", var);

   return val;
}

void EVAL::set_variable_value( SEXPR var, SEXPR val, SEXPR env )
{
   if (anyp(env))
      guard(env, envp);

   for (; anyp(env); env = getenvbase(env))
   {
      FRAME frame = getenvframe(env);  
      SEXPR vars = getframevars(frame);

      for ( int i = 0; anyp(vars); ++i, vars = getcdr(vars) )
      {
         if (getcar(vars) == var)
         {
            frameset(frame, i, val);
            return;
         }
      }
   }

   // global var
   set(var, val);
}

//
// Parse the Formal Parameters
//
//   parameter lists
//     (a ...)
//     tradition rest
//       (a . b) == (a #!rest b)
//

void EVAL::parse_formals( SEXPR formals, SEXPR& vars, BYTE& numv, BYTE& rargs )
{
   numv = 0;
   rargs = false;

   ListBuilder varlist;

   // validate and normalize the varlist
   while ( anyp(formals) )
   {
      numv++;

      if ( _symbolp(formals) )
      {
	 rargs = true;      
	 varlist.add( formals );
	 formals = null;
      }
      else
      {
	 varlist.add( guard(car(formals), symbolp) );
	 formals = cdr(formals);
      }
   }

   vars = varlist.get();
}

static void arg_error( const char* text, unsigned n1, unsigned n2, SEXPR fun )
{
   char msg[80];
   SPRINTF( msg, "%s -- actual=%u, expected=%u", text, n1, n2 );
   ERROR::severe( msg, fun );
}

SEXPR EVAL::extend_env_fun( SEXPR closure )
{
   //
   // extend the environment with the closure's vars
   // populate the frame with argstack values
   //

   // formal parameter attributes required:
   //   (<numv> <simple-var-list>)

   const auto nactual = static_cast<int>(argstack.getargc());
   const auto nformal = static_cast<int>(getclosurenumv(closure));
   const SEXPR benv = getclosurebenv(closure);
   const bool rargs = getclosurerargs(closure);

   // create an extended environment
   regstack.push( MEMORY::environment( nformal, getclosurevars(closure), benv ) );

   FRAME frame = getenvframe( regstack.top() );

   setframeclosure( frame, closure );

   if ( rargs == false ) 
   {
      // case I: no rest args
      //
      //   <fargs> := (a1 a2 ...)
      //
      if ( nactual != nformal )
      {
	 if (nactual < nformal)
	    arg_error( "too few arguments", nactual, nformal, closure );
	 else
	    arg_error( "too many arguments", nactual, nformal, closure );
      }
     
      int p = argstack.getfirstargindex();
     
      // BIND required
      for ( int i = 0; i < nactual; ++i )
	 frameset( frame, i, argstack[p++] );
   }
   else
   {
      // case II: rest arg
      //
      //   <fargs> := (a1 a2 ... aN-1 . aN)
      //
      const int nrequired = nformal - 1;

      if ( nactual < nrequired )
	 arg_error( "too few arguments", nactual, nrequired, closure );
     
      int p = argstack.getfirstargindex();
     
      // BIND required
      for ( int i = 0; i < nrequired; ++i )
	 frameset( frame, i, argstack[p++] );

      // BIND rest
      regstack.push(null);

      for ( int i = p + (nactual - nformal); i >= p; --i )
	 regstack.top() = cons( argstack[i], regstack.top() );
     
      frameset( frame, nrequired, regstack.pop() );
   }

   argstack.removeargc();

   return regstack.pop();
}

SEXPR EVAL::extend_env_vars( SEXPR bindings, SEXPR benv )
{
   //
   // extend the environment with let/letrec vars
   //   bindings = (binding ...)
   //   binding = (v e) | v
   //

   if ( nullp(bindings) )
      return benv;

   ListBuilder vars;
   int nvars = 0;

   while ( anyp(bindings) )
   {
      nvars++;
      SEXPR v = car(bindings);
      if ( consp(v) )
	 v = car(v);
      vars.add( v );
      bindings = cdr(bindings);
   }

   return MEMORY::environment( nvars, vars.get(), benv );
}

void EVAL::append( SEXPR env, SEXPR var, SEXPR val )
{
   FRAME frame = getenvframe(env);
   
   // I. prepend var to vars
   frame->vars = MEMORY::cons( var, frame->vars );

   // II. add a slot and assign val
   auto slot = new SEXPR[frame->nslots+1];
   slot[0] = val;
   if ( frame->slot )
   {
      for ( int i = 0; i < frame->nslots; ++i )
         slot[i+1] = frame->slot[i];
      delete[] frame->slot;
   }
   frame->slot = slot;
   frame->nslots += 1;
}

SEXPR EVAL::get_evaluator_state()
{
   const int rs_depth = regstack.getdepth();
   const int as_depth = argstack.getdepth();
   const int is_depth = intstack.getdepth();

   regstack.push( MEMORY::vector( rs_depth ) );
   for ( int i = 0; i < rs_depth; ++i )
      vectorset( regstack.top(), i, regstack[i] );

   regstack.push( MEMORY::vector( as_depth ) );
   for ( int i = 0; i < as_depth; ++i )
      vectorset( regstack.top(), i, argstack[i] );

   regstack.push( MEMORY::vector( is_depth ) );
   for ( int i = 0; i < is_depth; ++i )
      vectorset( regstack.top(), i, MEMORY::fixnum(intstack[i]) );

   SEXPR evs = MEMORY::vector(3);
   vectorset( evs, 2, regstack.pop() );
   vectorset( evs, 1, regstack.pop() );
   vectorset( evs, 0, regstack.pop() );
   
   return evs;
}

static void eval_marker()
{
   // mark the evaluator objects
   MEMORY::mark( argstack );
   MEMORY::mark( regstack );
   MEMORY::mark( EVAL::exp );
   MEMORY::mark( EVAL::env );
   MEMORY::mark( EVAL::aux );
   MEMORY::mark( EVAL::val );
   MEMORY::mark( EVAL::unev );
   MEMORY::mark( EVAL::map_code );
   MEMORY::mark( EVAL::for_code );
   MEMORY::mark( EVAL::rte_code );
   MEMORY::mark( EVAL::rtc_code );
   MEMORY::mark( EVAL::fep_code );
}

void EVAL::initialize()
{
   // evaluator registers
   exp = null;
   env = null;
   val = null;
   aux = null;
   unev = null;

   cont = EV_DONE;
   next = EV_DONE;

   theGlobalEnv = null;
   pc = 0;

   // set the special form dispatch value
   setform( symbol_quote,    EV_QUOTE );
   setform( symbol_delay,    EV_DELAY );
   setform( symbol_set,      EV_SET );
   setform( symbol_define,   EV_DEFINE );
   setform( symbol_if,       EV_IF );
   setform( symbol_cond,     EV_COND );
   setform( symbol_lambda,   EV_LAMBDA );
   setform( symbol_begin,    EV_BEGIN );
   setform( symbol_sequence, EV_BEGIN );
   setform( symbol_let,      EV_LET );
   setform( symbol_letrec,   EV_LETREC );
   setform( symbol_while,    EV_WHILE );
   setform( symbol_and,      EV_AND );
   setform( symbol_or,       EV_OR );
   setform( symbol_access,   EV_ACCESS );
   setform( null,            EV_APPLICATION );

   //
   // create code fragments
   //
   SEXPR map_bcodes = MEMORY::byte_vector( 5 );
   SEXPR for_bcodes = MEMORY::byte_vector( 5 );
   SEXPR rte_bcodes = MEMORY::byte_vector( 1 );
   SEXPR rtc_bcodes = MEMORY::byte_vector( 1 );
   SEXPR fep_bcodes = MEMORY::byte_vector( 2 );;

   bvecset( map_bcodes, 0, OP_MAP_INIT );
   bvecset( map_bcodes, 1, OP_MAP_APPLY );
   bvecset( map_bcodes, 2, OP_APPLY );
   bvecset( map_bcodes, 3, OP_MAP_RESULT );
   bvecset( map_bcodes, 4, OP_GOTO_CONT );

   bvecset( for_bcodes, 0, OP_FOR_INIT );
   bvecset( for_bcodes, 1, OP_FOR_APPLY );
   bvecset( for_bcodes, 2, OP_APPLY );
   bvecset( for_bcodes, 3, OP_FOR_RESULT );
   bvecset( for_bcodes, 4, OP_GOTO_CONT );

   bvecset( rte_bcodes, 0, OP_RTE );
   bvecset( rtc_bcodes, 0, OP_RTC );

   bvecset( fep_bcodes, 0, OP_FORCE_VALUE );
   bvecset( fep_bcodes, 1, OP_GOTO_CONT );


   map_code = MEMORY::code( map_bcodes, MEMORY::vector_null );
   for_code = MEMORY::code( for_bcodes, MEMORY::vector_null );
   rte_code = MEMORY::code( rte_bcodes, MEMORY::vector_null );
   rtc_code = MEMORY::code( rtc_bcodes, MEMORY::vector_null );
   fep_code = MEMORY::code( fep_bcodes, MEMORY::vector_null );

   SYMTAB::enter( "%%map-code", map_code );
   SYMTAB::enter( "%%for-code", for_code );
   SYMTAB::enter( "%%rte-code", rte_code );
   SYMTAB::enter( "%%rtc-code", rtc_code );
   SYMTAB::enter( "%%fep-code", fep_code );

   MEMORY::register_marker( eval_marker );
}

}
