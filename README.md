FLARESENSE - VERSION SIMPLE (LZ78 + TAMP, un CSV, tests simples)
=================================================================
Requisitos: Linux o WSL, gcc y make.   (Sin Python.)

COMPILAR (los dos algoritmos juntos, un solo programa)
    make
  o, sin make:
    gcc -O2 -std=gnu99 -Itamp -o simple simple.c compressors.c tamp/compressor.c tamp/decompressor.c tamp/common.c

USAR
    ./simple test                      # tests con datos boludos (todo a, todo b, 50/50, enteros...)
    ./simple test --size 5000          # lo mismo con otro largo
    ./simple ejemplo.csv               # comprime el archivo entero y cada fila por separado
    ./simple datos.csv --rows 10       # mensajes de 10 filas (simula tramas con varias muestras)
    ./simple datos.csv --show 20       # mostrar 20 mensajes en la tabla (def. 10)
    ./simple datos.csv --no-header     # si el CSV NO tiene encabezado
    ./simple trama.txt                 # cualquier archivo (ej. una trama bare-body ya armada)
    ./simple test -w 8 -l 8            # parametros de TAMP: ventana 8..15, literal (def. 10 y 8)

Para cada entrada: comprime con LZ78 y TAMP, descomprime, compara con el original
y muestra tamaños. Ahorro negativo = el compresor AGRANDO el dato.
Codigo de salida: 0 todo OK, 1 algun round-trip falla, 2 uso incorrecto.

ARCHIVOS
    simple.c        Programa principal (tests + modo CSV). Es el unico que se edita.
    compressors.c   Puente que expone LZ78 y TAMP sobre memoria. Copia del original,
                    con UNA linea cambiada: #include "lz78.c" (antes "../third_party/lz78/lz78.c").
    lz78.c          LZ78 original, sin tocar.
    tamp/           Libreria C de TAMP original, sin tocar.
    licencias/      Licencias de LZ78 y TAMP.
    ejemplo.csv     10 filas del ejemplo del README (encabezados aproximados).
    Makefile        make / make test / make clean

AGREGAR UN TEST
    En modo_test() de simple.c, antes de "free(b);", agregar por ejemplo:
        memset(b, 'a', N); b[N/2] = 'z';
        fallas += corre_caso("todo_a_con_una_z", b, N, 0);
    (el ultimo argumento en 1 exige ademas que ambos salgan en menos de la mitad.)
    Volver a correr "make" y "./simple test".
