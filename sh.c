// ===========================================================================
//  sh.c  --  Interprete de comandos (shell) para xv6
//
//  Proyecto 1 - Sistemas Operativos
//
//  Funcionalidades implementadas:
//    * Ejecucion de comandos simples con argumentos     ->  ls -l
//    * Redireccion de entrada                           ->  wc < archivo
//    * Redireccion de salida                            ->  ls > salida.txt
//    * Tuberias (pipes)                                 ->  cat a | wc -l
//    * Secuencias con ';'                               ->  echo uno ; echo dos
//    * Ejecucion en segundo plano con '&'               ->  sleep 50 &
//    * Comandos internos: cd, exit
//
//  Idea general:
//    1) LEER   una linea del teclado.
//    2) ANALIZAR esa linea y construir un ARBOL de comandos.
//    3) EJECUTAR ese arbol recursivamente usando fork/exec/pipe/dup.
//
//  El arbol es la parte clave. Por ejemplo, la linea
//
//        cat < entrada.txt | grep hola > salida.txt
//
//  se convierte en:
//
//                        PIPE
//                        |  |
//              REDIR(<) -+  +- REDIR(>)
//                 |             |
//              EXEC(cat)     EXEC(grep hola)
//
//  Ejecutar el nodo PIPE crea la tuberia y dos procesos hijos; cada hijo
//  ejecuta recursivamente su propio subarbol.
// ===========================================================================



#if !defined(XV6_RISCV) && !defined(XV6_X86)
#  if defined(__has_include)
#    if __has_include("kernel/types.h")
#      define XV6_RISCV 1
#    endif
#  endif
#endif

#ifdef XV6_RISCV
#  include "kernel/types.h"
#  include "kernel/stat.h"
#  include "kernel/fcntl.h"
#  include "user/user.h"
#  define SALIR(n)    exit(n)          // en xv6-riscv exit() recibe un codigo
#  define ESPERAR()   wait(0)          // y wait() recibe un puntero
#  define ERRF(...)   fprintf(2, __VA_ARGS__)
#  define OUTF(...)   printf(__VA_ARGS__)
#else
#  include "types.h"
#  include "stat.h"
#  include "fcntl.h"
#  include "user.h"
#  define SALIR(n)    exit()           
#  define ESPERAR()   wait()           
#  define ERRF(...)   printf(2, __VA_ARGS__)
#  define OUTF(...)   printf(1, __VA_ARGS__)
#endif

// Banderas para abrir el archivo destino de '>'.
#ifdef O_TRUNC
#  define MODO_ESCRITURA (O_WRONLY | O_CREATE | O_TRUNC)
#else
#  define MODO_ESCRITURA (O_WRONLY | O_CREATE)
#endif



//  Limites

#define MAX_ARGS   16     // maximo de argumentos por comando (incluye argv[0])
#define MAX_LINEA  128    // maximo de caracteres por linea de entrada



//  Tipos de nodo del arbol de comandos

#define TIPO_EXEC   1     // comando simple:  ls -l
#define TIPO_REDIR  2     // redireccion:     ... < arch   o   ... > arch
#define TIPO_PIPE   3     // tuberia:         izq | der
#define TIPO_LIST   4     // secuencia:       izq ; der
#define TIPO_BACK   5     // segundo plano:   cmd &

// Nodo generico. Todos los nodos empiezan con el campo 'tipo', asi que se
// puede mirar el tipo antes de convertir el puntero al struct concreto.
struct cmd {
  int tipo;
};

struct cmd_exec {
  int tipo;
  char *argv[MAX_ARGS];       // punteros al inicio de cada argumento
  char *fin_argv[MAX_ARGS];   // punteros al final de cada argumento
};

struct cmd_redir {
  int tipo;
  struct cmd *sub;            // comando al que se le aplica la redireccion
  char *archivo;              // nombre del archivo
  char *fin_archivo;
  int modo;                   // banderas para open()
  int fd;                     // descriptor a reemplazar: 0 = stdin, 1 = stdout
};

struct cmd_pipe {
  int tipo;
  struct cmd *izq;            // productor (escribe en el pipe)
  struct cmd *der;            // consumidor (lee del pipe)
};

struct cmd_list {
  int tipo;
  struct cmd *izq;
  struct cmd *der;
};

struct cmd_back {
  int tipo;
  struct cmd *sub;
};



//  Prototipos

struct cmd* analizar_cmd(char *s);
struct cmd* analizar_linea(char **ps, char *fin_s);
struct cmd* analizar_pipe(char **ps, char *fin_s);
struct cmd* analizar_exec(char **ps, char *fin_s);
struct cmd* analizar_redir(struct cmd *cmd, char **ps, char *fin_s);
struct cmd* terminar_cadenas(struct cmd *cmd);
void        ejecutar_cmd(struct cmd *cmd);
int         fork1(void);



//  CONSTRUCTORES DE NODOS
//  Cada uno reserva memoria con malloc, la pone en cero (para que los
//  punteros que no se usen queden en NULL) y rellena los campos.


struct cmd*
crear_exec(void)
{
  struct cmd_exec *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->tipo = TIPO_EXEC;
  return (struct cmd*)cmd;
}

struct cmd*
crear_redir(struct cmd *sub, char *archivo, char *fin_archivo, int modo, int fd)
{
  struct cmd_redir *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->tipo = TIPO_REDIR;
  cmd->sub = sub;
  cmd->archivo = archivo;
  cmd->fin_archivo = fin_archivo;
  cmd->modo = modo;
  cmd->fd = fd;
  return (struct cmd*)cmd;
}

struct cmd*
crear_pipe(struct cmd *izq, struct cmd *der)
{
  struct cmd_pipe *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->tipo = TIPO_PIPE;
  cmd->izq = izq;
  cmd->der = der;
  return (struct cmd*)cmd;
}

struct cmd*
crear_list(struct cmd *izq, struct cmd *der)
{
  struct cmd_list *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->tipo = TIPO_LIST;
  cmd->izq = izq;
  cmd->der = der;
  return (struct cmd*)cmd;
}

struct cmd*
crear_back(struct cmd *sub)
{
  struct cmd_back *cmd;

  cmd = malloc(sizeof(*cmd));
  memset(cmd, 0, sizeof(*cmd));
  cmd->tipo = TIPO_BACK;
  cmd->sub = sub;
  return (struct cmd*)cmd;
}



//  TOKENIZADOR (analisis lexico)
//  Convierte la linea de texto en "tokens". Un token puede ser:
//     'a'  -> una palabra (nombre de programa, argumento o nombre de archivo)
//     '|'  '<'  '>'  ';'  '&'   -> un simbolo especial
//      0   -> fin de la linea
//
//  IMPORTANTE: no se copian cadenas. Se devuelven punteros al inicio y al
//  final de cada palabra dentro del buffer original. Los caracteres '\0'
//  se colocan al final, en terminar_cadenas().


char espacios[] = " \t\r\n\v";
char simbolos[] = "<|>;&";

int
obtener_token(char **ps, char *fin_s, char **inicio, char **fin)
{
  char *s;
  int tok;

  s = *ps;
  while(s < fin_s && strchr(espacios, *s))   // saltar espacios iniciales
    s++;
  if(inicio)
    *inicio = s;

  tok = *s;
  switch(*s){
  case 0:
    break;
  case '|':
  case '<':
  case '>':
  case ';':
  case '&':
    s++;                                     // los simbolos son de 1 caracter
    break;
  default:
    tok = 'a';                               // es una palabra
    while(s < fin_s && !strchr(espacios, *s) && !strchr(simbolos, *s))
      s++;
    break;
  }

  if(fin)
    *fin = s;

  while(s < fin_s && strchr(espacios, *s))   // saltar espacios finales
    s++;
  *ps = s;
  return tok;
}

// Mira (sin consumir) si el siguiente token es uno de los caracteres dados.
int
buscar(char **ps, char *fin_s, char *tokens)
{
  char *s;

  s = *ps;
  while(s < fin_s && strchr(espacios, *s))
    s++;
  *ps = s;
  return *s && strchr(tokens, *s) != 0;
}



//  ANALIZADOR SINTACTICO (parser)
//  Gramatica que se implementa (descenso recursivo):
//     linea   ::=  pipe  [ '&' ]  [ ';' linea ]
//     pipe    ::=  exec  [ '|' pipe ]
//     exec    ::=  { redir | palabra }
//     redir   ::=  ( '<' | '>' ) palabra
//  Cada funcion devuelve un nodo del arbol, o 0 si hubo un error de sintaxis.


struct cmd*
analizar_cmd(char *s)
{
  char *fin_s;
  struct cmd *cmd;

  fin_s = s + strlen(s);
  cmd = analizar_linea(&s, fin_s);
  if(cmd == 0)
    return 0;

  buscar(&s, fin_s, "");                      // saltar espacios sobrantes
  if(s != fin_s){
    ERRF("sh: no se entendio la parte final: %s\n", s);
    return 0;
  }
  terminar_cadenas(cmd);
  return cmd;
}

struct cmd*
analizar_linea(char **ps, char *fin_s)
{
  struct cmd *cmd, *der;

  cmd = analizar_pipe(ps, fin_s);
  if(cmd == 0)
    return 0;

  // '&' -> ejecutar en segundo plano (no esperar al hijo)
  while(buscar(ps, fin_s, "&")){
    obtener_token(ps, fin_s, 0, 0);
    cmd = crear_back(cmd);
  }

  // ';' -> ejecutar el siguiente comando despues de este
  if(buscar(ps, fin_s, ";")){
    obtener_token(ps, fin_s, 0, 0);
    der = analizar_linea(ps, fin_s);
    if(der == 0)
      return 0;
    cmd = crear_list(cmd, der);
  }
  return cmd;
}

struct cmd*
analizar_pipe(char **ps, char *fin_s)
{
  struct cmd *cmd, *der;

  cmd = analizar_exec(ps, fin_s);
  if(cmd == 0)
    return 0;

  if(buscar(ps, fin_s, "|")){
    obtener_token(ps, fin_s, 0, 0);
    der = analizar_pipe(ps, fin_s);          // recursion: a | b | c
    if(der == 0)
      return 0;
    cmd = crear_pipe(cmd, der);
  }
  return cmd;
}

// Envuelve 'cmd' en tantos nodos REDIR como redirecciones encuentre.
struct cmd*
analizar_redir(struct cmd *cmd, char **ps, char *fin_s)
{
  int tok;
  char *q, *eq;

  while(buscar(ps, fin_s, "<>")){
    tok = obtener_token(ps, fin_s, 0, 0);    // consume el '<' o el '>'

    if(obtener_token(ps, fin_s, &q, &eq) != 'a'){
      ERRF("sh: falta el nombre del archivo despues de '%c'\n", tok);
      return 0;
    }

    if(tok == '<')
      cmd = crear_redir(cmd, q, eq, O_RDONLY, 0);        // 0 = entrada
    else
      cmd = crear_redir(cmd, q, eq, MODO_ESCRITURA, 1);  // 1 = salida
  }
  return cmd;
}

struct cmd*
analizar_exec(char **ps, char *fin_s)
{
  char *q, *eq;
  int tok, argc;
  struct cmd_exec *ecmd;
  struct cmd *ret;

  ret = crear_exec();
  ecmd = (struct cmd_exec*)ret;
  argc = 0;

  // Una redireccion puede aparecer antes del comando:  < entrada.txt wc
  ret = analizar_redir(ret, ps, fin_s);
  if(ret == 0)
    return 0;

  while(!buscar(ps, fin_s, "|;&")){
    tok = obtener_token(ps, fin_s, &q, &eq);
    if(tok == 0)                              // fin de la linea
      break;
    if(tok != 'a'){
      ERRF("sh: error de sintaxis cerca de '%c'\n", tok);
      return 0;
    }

    if(argc >= MAX_ARGS - 1){                 // -1: hay que dejar sitio al NULL
      ERRF("sh: demasiados argumentos (maximo %d)\n", MAX_ARGS - 1);
      return 0;
    }
    ecmd->argv[argc] = q;
    ecmd->fin_argv[argc] = eq;
    argc++;

    // ... o despues, o en medio:  wc < entrada.txt > salida.txt
    ret = analizar_redir(ret, ps, fin_s);
    if(ret == 0)
      return 0;
  }

  ecmd->argv[argc] = 0;                       // exec() exige argv terminado en NULL
  ecmd->fin_argv[argc] = 0;
  return ret;
}

// Durante el analisis solo se guardaron punteros a inicio/fin de cada palabra.
// Ahora que ya no hay que seguir leyendo la linea, se escribe un '\0' al final
// de cada palabra para convertirlas en cadenas de C de verdad.
struct cmd*
terminar_cadenas(struct cmd *cmd)
{
  int i;

  if(cmd == 0)
    return 0;

  switch(cmd->tipo){
  case TIPO_EXEC:
    {
      struct cmd_exec *ecmd = (struct cmd_exec*)cmd;
      for(i = 0; ecmd->argv[i]; i++)
        *ecmd->fin_argv[i] = 0;
    }
    break;

  case TIPO_REDIR:
    {
      struct cmd_redir *rcmd = (struct cmd_redir*)cmd;
      terminar_cadenas(rcmd->sub);
      *rcmd->fin_archivo = 0;
    }
    break;

  case TIPO_PIPE:
    {
      struct cmd_pipe *pcmd = (struct cmd_pipe*)cmd;
      terminar_cadenas(pcmd->izq);
      terminar_cadenas(pcmd->der);
    }
    break;

  case TIPO_LIST:
    {
      struct cmd_list *lcmd = (struct cmd_list*)cmd;
      terminar_cadenas(lcmd->izq);
      terminar_cadenas(lcmd->der);
    }
    break;

  case TIPO_BACK:
    {
      struct cmd_back *bcmd = (struct cmd_back*)cmd;
      terminar_cadenas(bcmd->sub);
    }
    break;
  }
  return cmd;
}



//  EJECUCION
//  ejecutar_cmd() SIEMPRE se llama dentro de un proceso hijo y NUNCA regresa:
//  termina llamando a exec() o a exit(). Eso simplifica mucho el codigo,
//  porque cada rama del arbol puede "adueñarse" de su proceso.


// fork() que aborta si falla, para no tener que comprobarlo en cada sitio.
int
fork1(void)
{
  int pid;

  pid = fork();
  if(pid == -1){
    ERRF("sh: no se pudo crear el proceso (fork fallo)\n");
    SALIR(1);
  }
  return pid;
}

void
ejecutar_cmd(struct cmd *cmd)
{
  int p[2];

  // Solo puede llegar un puntero nulo si el analisis de la linea fallo.
  // En ese caso se regresa y quien llamo se encarga de terminar el proceso.
  if(cmd == 0)
    return;

  switch(cmd->tipo){

  //  COMANDO SIMPLE
  //  exec() reemplaza la imagen del proceso actual por el programa pedido.
  //  Si tiene exito NO regresa; si regresa, es que fallo.
  case TIPO_EXEC:
    {
      struct cmd_exec *ecmd = (struct cmd_exec*)cmd;
      if(ecmd->argv[0] == 0)                  // linea vacia
        SALIR(0);
      exec(ecmd->argv[0], ecmd->argv);
      ERRF("sh: no se encontro el comando '%s'\n", ecmd->argv[0]);
      SALIR(1);
    }
    break;

  //  REDIRECCION
  //  Truco clave de Unix: open() siempre devuelve el descriptor LIBRE MAS
  //  PEQUEÑO. Si cerramos el 0 (o el 1) justo antes, el archivo que abrimos
  //  ocupa exactamente ese numero, y el programa que ejecutemos despues
  //  creera que esta leyendo del teclado / escribiendo en pantalla.
  case TIPO_REDIR:
    {
      struct cmd_redir *rcmd = (struct cmd_redir*)cmd;
      close(rcmd->fd);
      if(open(rcmd->archivo, rcmd->modo) < 0){
        ERRF("sh: no se pudo abrir el archivo '%s'\n", rcmd->archivo);
        SALIR(1);
      }
      ejecutar_cmd(rcmd->sub);                // sigue con el comando envuelto
    }
    break;

  //  TUBERIA
  //  pipe(p) crea un canal:  p[0] = extremo de LECTURA, p[1] = ESCRITURA.
  //  Se crean dos hijos: el izquierdo escribe en p[1] como si fuera stdout,
  //  el derecho lee de p[0] como si fuera stdin.
  //
  //  Cerrar los extremos que NO se usan es obligatorio: mientras exista
  //  algun proceso con el extremo de escritura abierto, el lector se queda
  //  esperando para siempre y el shell se cuelga.

  case TIPO_PIPE:
    {
      struct cmd_pipe *pcmd = (struct cmd_pipe*)cmd;

      if(pipe(p) < 0){
        ERRF("sh: no se pudo crear la tuberia\n");
        SALIR(1);
      }

      if(fork1() == 0){                       // hijo IZQUIERDO (productor)
        close(1);                             // liberar stdout
        if(dup(p[1]) != 1){                   // el extremo de escritura pasa a ser el 1
          ERRF("sh: fallo dup en la tuberia\n");
          SALIR(1);
        }
        close(p[0]);
        close(p[1]);
        ejecutar_cmd(pcmd->izq);
        SALIR(1);
      }

      if(fork1() == 0){                       // hijo DERECHO (consumidor)
        close(0);                             // liberar stdin
        if(dup(p[0]) != 0){                   // el extremo de lectura pasa a ser el 0
          ERRF("sh: fallo dup en la tuberia\n");
          SALIR(1);
        }
        close(p[0]);
        close(p[1]);
        ejecutar_cmd(pcmd->der);
        SALIR(1);
      }

      close(p[0]);                            // el padre no usa la tuberia
      close(p[1]);
      ESPERAR();                              // esperar a los dos hijos
      ESPERAR();
    }
    break;

  //  SECUENCIA  ( ; )
  case TIPO_LIST:
    {
      struct cmd_list *lcmd = (struct cmd_list*)cmd;
      if(fork1() == 0){
        ejecutar_cmd(lcmd->izq);
        SALIR(1);
      }
      ESPERAR();
      ejecutar_cmd(lcmd->der);
    }
    break;

  //  SEGUNDO PLANO  ( & )
  //  Se crea el hijo pero NO se espera por el.
  case TIPO_BACK:
    {
      struct cmd_back *bcmd = (struct cmd_back*)cmd;
      if(fork1() == 0){
        ejecutar_cmd(bcmd->sub);
        SALIR(1);
      }
    }
    break;

  default:
    ERRF("sh: tipo de comando desconocido\n");
    SALIR(1);
  }

  SALIR(0);
}


//  LECTURA DE LA LINEA Y COMANDOS INTERNOS

// Lee una linea de la entrada estandar, sin el '\n' final.
// Devuelve el numero de caracteres leidos, o -1 si se acabo la entrada.
int
leer_linea(char *buf, int max)
{
  int i, n;
  char c;

  OUTF("$ ");                                 // prompt

  i = 0;
  while(i < max - 1){
    n = read(0, &c, 1);
    if(n < 1){                                // fin de la entrada (Ctrl-D)
      if(i == 0)
        return -1;
      break;
    }
    if(c == '\n')
      break;
    buf[i++] = c;
  }
  buf[i] = 0;
  return i;
}

// Comprueba si 'linea' empieza con la palabra 'palabra'.
int
empieza_con(char *linea, char *palabra)
{
  while(*palabra){
    if(*linea != *palabra)
      return 0;
    linea++;
    palabra++;
  }
  return *linea == 0 || *linea == ' ' || *linea == '\t';
}


//  Los comandos internos NO pueden ejecutarse con fork+exec.
//  'cd' es el ejemplo clasico: chdir() cambia el directorio del proceso que
//  la llama. Si lo hicieramos en un hijo, el hijo cambiaria de directorio,
//  moriria, y el shell (el padre) seguiria exactamente donde estaba.
//  Por eso 'cd' se ejecuta en el propio proceso del shell.
//  Devuelve 1 si la linea era un comando interno y ya fue atendida.

int
comando_interno(char *linea)
{
  char *p, *arg, *fin;

  p = linea;
  while(*p == ' ' || *p == '\t')
    p++;

  if(empieza_con(p, "exit"))
    SALIR(0);

  if(empieza_con(p, "cd")){
    arg = p + 2;
    while(*arg == ' ' || *arg == '\t')
      arg++;

    fin = arg + strlen(arg);                  // recortar espacios finales
    while(fin > arg && (fin[-1] == ' ' || fin[-1] == '\t'))
      fin--;
    *fin = 0;

    if(*arg == 0)
      return 1;                               // 'cd' sin argumento: no hace nada
    if(chdir(arg) < 0)
      ERRF("sh: no se pudo entrar en '%s'\n", arg);
    return 1;
  }

  return 0;
}



//  BUCLE PRINCIPAL


int
main(void)
{
  static char buf[MAX_LINEA];
  int fd;

  // Asegurar que los descriptores 0, 1 y 2 (entrada, salida y error)
  // esten abiertos y apunten a la consola.
  while((fd = open("console", O_RDWR)) >= 0){
    if(fd >= 3){
      close(fd);
      break;
    }
  }

  while(leer_linea(buf, sizeof(buf)) >= 0){
    if(buf[0] == 0)                           // linea vacia: no hacer nada
      continue;

    if(comando_interno(buf))                  // cd / exit: sin fork
      continue;

    // Se analiza la linea DENTRO del hijo a proposito: toda la memoria que
    // reserva el parser con malloc desaparece sola cuando el hijo termina,
    // asi el shell no acumula memoria con cada comando.
    if(fork1() == 0){
      ejecutar_cmd(analizar_cmd(buf));
      SALIR(1);          // solo se llega aqui si la linea tenia un error
    }

    ESPERAR();
  }

  SALIR(0);
  return 0;
}
