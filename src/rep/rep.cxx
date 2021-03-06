#include "rep.hxx"

#include <cstdio>
#include <cstdlib>

#include <string>

#include "core/error.hxx"
#include "core/reader.hxx"
#include "core/pio.hxx"
#include "core/printer.hxx"
#include "core/memory.hxx"
#include "core/symtab.hxx"
#include "core/transcript.hxx"
#include "core/regstack.hxx"

#include "eval/eval.hxx"
#ifdef BYTE_CODE_EVALUATOR
#include "eval/imager.hxx"
#endif

namespace escheme
{
   
extern int unix_argc;
extern char** unix_argv;

// symbol names for hanging sexprs
const char* SYSTEM_REPLOOP = "*system-rep-loop*";
const char* SYSTEM_LOADER  = "*system-loader*";
const char* SYSTEM_PATH    = "*system-path*";
const char* TOPLEVEL       = "*toplevel*";
const char* REP_LOOP       = "*rep-loop*";

static void define_system()
{
   const std::string system = R"(
(begin
   (define *version* "<interpreter>")
   (set-prompt "noise> ")
   (define *rep-loop*
     (lambda ()
       (while #t
         (let ((sexpr (read *terminal*)))
           (add-history sexpr)
           (print (eval sexpr))))))
   (let ((x 0))
     (call/cc (lambda (cc) (set! *toplevel* cc)))
     (if (= x 0)
       (begin
         (set! x 1)
         (load (system-path "escheme.scm"))
          )))
     (display "escheme ")
     (display *version*)
     (newline)
     (newline)
     (flush-output)
     (call/cc (lambda (cc) (set! *toplevel* cc)))
     (*rep-loop*))

(define (load file . noisily)
  (if (not (string? file))
      (error "filename is not a string")
      (let ((port (open-input-file file)))
        (if port
          (let ((sexpr (read port)))
            (while (not (eof-object? sexpr))
              (if noisily (begin (display ">> ") (print sexpr)))
	      (eval sexpr)
	      (set! sexpr (read port)))
            (close-port port)))
        port)))

(define (system-path file)
  (let ((home (getenv "ESCHEME")))
    (if (= (string-length home) 0)
        file
        (string-append home "/" file))))
)";

   const SEXPR port = PIO::open_on_string( MEMORY::string(system), pm_input );
   
   GcSuspension gcs( "define-system" );
   
   setvalue( SYMTAB::enter(SYSTEM_REPLOOP), READER::read(port) );
   setvalue( SYMTAB::enter(SYSTEM_LOADER), READER::read(port) );
   setvalue( SYMTAB::enter(SYSTEM_PATH), READER::read(port) );
}

#ifdef BYTE_CODE_EVALUATOR
static std::string system_path( const char* file )
{
   const char* home = ::getenv( "ESCHEME" );
   if ( home )
      return std::string(home) + "/" + file;
   else
      return file;
}
#endif

void rep_loop()
{
   // build the "system"

   try
   {
#ifdef BYTE_CODE_EVALUATOR
      bool load_compiler = true;
      if ( escheme::unix_argc > 1 )
      {
	 std::string arg1 = escheme::unix_argv[1];
	 if ( arg1 == "-i" || arg1 == "--interpreter" )
	    load_compiler = false;
      }

      if ( load_compiler )
	 IMAGER::image_load( system_path("compiler/compiler-image.scm") );
#endif
      define_system();
      
      EVAL::eceval( getvalue(SYMTAB::enter(SYSTEM_LOADER)) );
      EVAL::eceval( getvalue(SYMTAB::enter(SYSTEM_PATH)) );
   }
   catch (...)
   {
      printf("Error during system definition. Terminating.\n");
      return;
   }

   //
   // REP Loop
   //   a single call into the interpreter.
   //   exit on exceptions and evaluate the toplevel continuation.
   //

   SEXPR exp = getvalue( SYMTAB::enter(SYSTEM_REPLOOP) );

   while ( true )
   {
      try
      {
	 EVAL::eceval( exp );
	 return;
      }
      catch ( ERROR::SevereError& )
      {
	 exp = SYMTAB::enter(TOPLEVEL);

	 const SEXPR val = getvalue(exp);
	 
	 if ( contp(val) ||
	      funcp(val) ||
	      specialp(val) ||
	      closurep(val) )
	 {
	    // make it into an application
	    exp = MEMORY::cons( exp, null );
	 }
	 else
	 {
	    // abandon the interpreter
	    printf( "toplevel is unbound\n" );
	    return;
	 }
      }
      catch ( ERROR::FatalError& )
      {
	 printf( "handling fatal error\n" );
	 return;
      }
      catch ( ERROR::Exit& )
      {
	 // place holder for Exit actions
	 TRANSCRIPT::off();
	 return;
      }
      catch ( ... )
      {
	 printf( "handling other error\n" );
	 return;
      }
   }
}

}
