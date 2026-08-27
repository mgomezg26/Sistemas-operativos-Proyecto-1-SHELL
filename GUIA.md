# Proyecto 1 — Shell para xv6-riscv

Guía de compilación, ejecución y explicación del código.
**Escrita solo para xv6-riscv** (la versión actual del MIT, la que tiene carpetas `kernel/` y `user/`).

**Archivos entregados**

| Archivo | Qué es |
|---|---|
| `sh.c` | El intérprete de comandos completo, escrito desde cero y comentado |
| `pruebas.txt` | Batería de 18 pruebas que se ejecuta dentro de xv6 |
| `GUIA.md` | Este documento |

---

## Paso 1 — Instalar el toolchain

Necesitas dos cosas: un compilador cruzado de RISC-V (tu computador es x86 o ARM, xv6 es RISC-V) y QEMU para emular la máquina.

### Ubuntu · Debian · WSL2

```bash
sudo apt-get update
sudo apt-get install -y git build-essential gdb-multiarch \
    qemu-system-misc gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu
```

### macOS

```bash
brew tap riscv-software-src/riscv
brew install riscv-tools qemu
```

### Windows

No compiles xv6 nativo. Abre PowerShell como administrador, ejecuta `wsl --install -d Ubuntu`, reinicia, y desde ahí sigue los pasos de Ubuntu **dentro** de WSL.

### Comprobar que quedó bien

```bash
riscv64-linux-gnu-gcc --version
qemu-system-riscv64 --version
```

Las dos deben imprimir una versión. Si alguna dice `command not found`, no sigas: repite la instalación.

> El Makefile de xv6-riscv busca el compilador probando varios prefijos (`riscv64-unknown-elf-`, `riscv64-linux-gnu-`, …), así que el paquete de Ubuntu le sirve sin tocar nada.

---

## Paso 2 — Conseguir xv6-riscv y probarlo virgen

Si tu profesor te dio la carpeta, úsala. Si no:

```bash
git clone https://github.com/mit-pdos/xv6-riscv.git
cd xv6-riscv
```

Antes de tocar nada, comprueba que arranca de fábrica:

```bash
make qemu
```

Debe aparecer:

```
xv6 kernel is booting

init: starting sh
$
```

**Para salir de QEMU:** pulsa `Ctrl-a`, suéltalo, y luego pulsa `x`.

Este paso no es opcional. Si xv6 no arranca limpio, cualquier error posterior te va a confundir.

---

## Paso 3 — Instalar tu `sh.c`

Desde la raíz de `xv6-riscv`:

```bash
cp user/sh.c user/sh.c.original      # guarda el original por si acaso
cp /ruta/donde/descargaste/sh.c user/sh.c
```

**No hay que tocar el Makefile para esto.** El shell ya está en la lista `UPROGS`, así que al reemplazar el archivo se compila solo.

---

## Paso 4 — Meter `pruebas.txt` en el disco de xv6

xv6 no comparte disco con tu computador: los archivos se empaquetan en la imagen `fs.img` al compilar.

Copia el archivo a la raíz del proyecto:

```bash
cp /ruta/donde/descargaste/pruebas.txt .
```

Ahora mira cómo es tu regla `fs.img` (cambia un poco entre versiones):

```bash
grep -n -A 1 "^fs.img:" Makefile
```

### Si tu regla tiene `$(UEXTRA)`

Es la ruta fácil. Busca la línea donde se define `UEXTRA` y añade el archivo al final:

```make
UEXTRA=user/xargstest.sh pruebas.txt
```

### Si tu regla NO tiene `$(UEXTRA)`

Edita la regla y añade `pruebas.txt` en las **dos** líneas:

```make
fs.img: mkfs/mkfs README pruebas.txt $(UPROGS)
	mkfs/mkfs fs.img README pruebas.txt $(UPROGS)
```

> **Cuidado con el Makefile:** la segunda línea tiene que empezar con un **TAB real**, no con espacios. Si tu editor convierte tabs en espacios, `make` fallará con `missing separator`. En VS Code, mira abajo a la derecha y cambia "Spaces" por "Tab Size → Indent Using Tabs" antes de editar.

---

## Paso 5 — Compilar y arrancar

```bash
make clean
make qemu
```

El `make clean` es importante: sin él, `fs.img` no se reconstruye y `pruebas.txt` no aparece dentro de xv6.

---

## Paso 6 — Ejecutar las pruebas

Ya en el prompt `$` de xv6, primero confirma que el archivo llegó:

```
$ ls
```

Debes ver `pruebas.txt` en la lista. Entonces:

```
$ sh < pruebas.txt
```

Esto ejecuta las 18 pruebas de corrido, y de paso demuestra la redirección de entrada: el shell se está alimentando a sí mismo desde un archivo.

Para la sustentación conviene también escribirlas a mano, una a una:

```
$ echo hola mundo
$ echo alfa beta > salida.txt
$ cat salida.txt
$ cat < salida.txt
$ cat salida.txt | wc
$ ls | grep salida
$ cat < salida.txt | wc > conteo.txt
$ cat conteo.txt
$ comando_que_no_existe
$ cd /
$ exit
```

---

## Todo de una

Si ya tienes el toolchain del paso 1 instalado, esto es la secuencia completa:

```bash
git clone https://github.com/mit-pdos/xv6-riscv.git
cd xv6-riscv
cp user/sh.c user/sh.c.original
cp ~/Descargas/sh.c user/sh.c
cp ~/Descargas/pruebas.txt .
# --- editar el Makefile como dice el Paso 4 ---
make clean
make qemu
# ya dentro de xv6:   sh < pruebas.txt
# para salir:         Ctrl-a  luego  x
```

---

## Cómo funciona el shell

El programa hace lo mismo en cada vuelta del bucle: **leer → analizar → ejecutar**.

### 1. Leer (`leer_linea`)

Lee de la entrada estándar carácter por carácter con `read(0, &c, 1)` hasta encontrar un `\n`. Si `read` devuelve 0, es fin de entrada (Ctrl-D o final del archivo de pruebas) y el shell termina.

### 2. Analizar (`analizar_cmd` y compañía)

Aquí está el corazón del proyecto. La línea de texto se convierte en un **árbol de comandos**. Por ejemplo:

```
cat < entrada.txt | grep hola > salida.txt
```

se convierte en:

```
                PIPE
                |  |
      REDIR(<) -+  +- REDIR(>)
         |             |
      EXEC(cat)     EXEC(grep hola)
```

El análisis se hace en dos capas:

- **Tokenizador** (`obtener_token`): parte la línea en piezas. Devuelve `'a'` si la pieza es una palabra, o el propio carácter si es un símbolo (`< > | ; &`).
- **Parser de descenso recursivo** (`analizar_linea` → `analizar_pipe` → `analizar_exec` → `analizar_redir`): una función por cada regla de la gramática. Cada una llama a la de abajo y, si encuentra su símbolo, envuelve el resultado en un nodo nuevo.

Un detalle importante: el parser **no copia cadenas**. Guarda punteros al inicio y al final de cada palabra dentro del buffer original. Solo al terminar, `terminar_cadenas` escribe los `\0`. Esto se hace así porque poner el `\0` antes destruiría el resto de la línea que aún falta por leer.

### 3. Ejecutar (`ejecutar_cmd`)

Esta función **siempre corre dentro de un proceso hijo y nunca regresa**: termina en `exec()` o en `exit()`. Eso permite que cada rama del árbol se "adueñe" de su propio proceso.

**Comando simple.** `exec(argv[0], argv)` reemplaza la imagen del proceso por el programa pedido. Si `exec` tiene éxito no regresa nunca; si regresa, es que falló, y hay que imprimir el error y morir (si no, tendrías dos shells vivos).

**Redirección.** Se apoya en una garantía del estándar Unix: `open()` devuelve **siempre el descriptor libre más pequeño**. Entonces:

```c
close(1);                    // libera la salida estándar
open("salida.txt", ...);     // el archivo recibe forzosamente el número 1
```

El programa que se ejecute después escribirá en el descriptor 1 creyendo que es la pantalla, y en realidad irá al archivo. No hay que modificar el programa: es el shell el que le cambia el mundo antes de arrancarlo.

**Tuberías.** `pipe(p)` crea un canal con dos extremos: `p[0]` para leer y `p[1]` para escribir. Se crean dos hijos:

- El **izquierdo** hace `close(1); dup(p[1]);` → su salida estándar es el pipe.
- El **derecho** hace `close(0); dup(p[0]);` → su entrada estándar es el pipe.

Después, **todos** cierran los extremos que no usan, incluido el padre. Esto no es opcional: mientras exista algún proceso con el extremo de escritura abierto, el lector se queda esperando datos que nunca llegarán y el shell se congela. Es el error número uno en este proyecto.

Finalmente el padre hace dos `wait()`, uno por cada hijo.

### 4. Comandos internos

`cd` **no puede** ejecutarse con `fork` + `exec`. `chdir()` cambia el directorio del proceso que la llama; si lo hiciera un hijo, el hijo cambiaría de carpeta, moriría, y el shell seguiría exactamente donde estaba. Por eso `cd` (y `exit`) se atienden en el proceso del propio shell, antes de hacer `fork`.

### Por qué hay que escribir `/echo` en vez de `echo` después de un `cd`

Esto no es una limitación de tu shell: es un comportamiento real de xv6. A diferencia de Linux, xv6 **no tiene `$PATH`**. Cuando `exec()` recibe `"echo"`, simplemente intenta resolverlo como una ruta relativa al directorio actual — no busca en ninguna lista de carpetas. Mientras tu directorio actual es `/`, `echo` funciona porque el archivo `echo` está justo ahí. En cuanto haces `cd p_dir`, el directorio actual pasa a ser `/p_dir`, que no tiene ningún archivo llamado `echo`, así que `exec()` falla con "no encontrado" — exactamente igual que le pasaría al `sh.c` original de xv6.

La solución es usar la ruta absoluta: `/echo`, `/ls`, `/cat`, etc. Por eso `pruebas.txt` usa `/echo` y `/ls` en el caso de `cd` (Prueba 17): es la forma correcta de invocar un programa cuando no estás en la raíz del sistema de archivos. Vale la pena mencionar esto en la sustentación — muestra que entendiste que `exec` en xv6 resuelve por `namei`, sin búsqueda en PATH.

### Nota de diseño: por qué se analiza dentro del hijo

En `main`, el `fork` ocurre **antes** de llamar al parser. Toda la memoria que el parser reserva con `malloc` desaparece sola cuando el hijo termina, así que el shell nunca acumula memoria aunque no exista `free`. Es la misma estrategia del xv6 original y es un punto que vale la pena mencionar en la sustentación.

---

## Funcionalidades y límites

**Implementado**

- Comandos simples con argumentos
- Redirección de entrada `<` y de salida `>` (en cualquier posición de la línea)
- Tuberías `|`, encadenables (`a | b | c`)
- Secuencias con `;`
- Ejecución en segundo plano con `&`
- Comandos internos `cd` y `exit`
- Mensajes de error claros que **no** matan el shell

**No implementado** (dilo tú antes de que lo pregunten)

- Comillas y escapes: `echo "hola mundo"` trata las comillas como parte del texto
- Append `>>` — xv6 no tiene `O_APPEND`
- Subshells con paréntesis `( ... )`
- Variables de entorno y expansión de comodines (`*`), que en Unix real hace el shell
- Máximo 16 argumentos y 128 caracteres por línea (constantes `MAX_ARGS` y `MAX_LINEA` al inicio del archivo, fáciles de subir)

---

## Problemas comunes

| Síntoma | Causa y solución |
|---|---|
| `riscv64-linux-gnu-gcc: command not found` | Falta el compilador cruzado. Repite el paso 1. |
| `qemu-system-riscv64: command not found` | Falta `qemu-system-misc`. En algunas versiones de Ubuntu el paquete es `qemu-system-riscv64`. |
| `Makefile: missing separator` | Usaste espacios en vez de un TAB al editar la regla `fs.img`. |
| Dentro de xv6, `ls` no muestra `pruebas.txt` | Faltó `make clean`, o el nombre no quedó en la regla `fs.img`. |
| El shell se cuelga al usar `\|` | Falta cerrar algún extremo del pipe. Revisa que padre e hijos cierren `p[0]` y `p[1]`. |
| El prompt `$` aparece dos veces por comando | Normal al usar `sh < pruebas.txt`: son el shell padre y el shell hijo imprimiendo. |
| QEMU arranca pero no responde el teclado | Estás en la ventana gráfica. Usa `make qemu` desde una terminal normal, no desde un IDE con terminal limitada. |
| No puedo salir de QEMU | `Ctrl-a`, soltar, `x`. |
| Después de `cd carpeta`, `echo` o `ls` dicen "no se encontro el comando" | Normal en xv6: no hay `$PATH`. Usa la ruta absoluta (`/echo`, `/ls`) cuando no estés en `/`. |

---

## Verificación

El código se compiló con `gcc -Wall -Werror -O2` **sin un solo warning**, y las 18 pruebas de `pruebas.txt` se ejecutaron dentro de QEMU con salida correcta: comandos simples, `<`, `>`, tuberías de 2 y 3 etapas, combinaciones de tubería con redirección, `;`, `cd`, comando inexistente y error de sintaxis (en ambos casos el shell imprime el error y sigue vivo, sin colgarse).

> `sh.c` incluye unas macros al inicio que detectan automáticamente si estás en xv6-riscv o en el xv6 antiguo de x86. Con xv6-riscv se activa sola la rama correcta; no tienes que hacer nada. Si tu profesor prefiere el archivo sin esa detección, se puede entregar una versión limpia solo para RISC-V.
