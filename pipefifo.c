#define _GNU_SOURCE // Wymagane dla TEMP_FAILURE_RETRY
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/wait.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#define ERR(source) \
    (fprintf(stderr, "%s:%d\n", __FILE__, __LINE__), perror(source), kill(0, SIGKILL), exit(EXIT_FAILURE))
/* =====================================================================================
 * SPIS FUNKCJI
 * =====================================================================================
 *
 * --- KOMUNIKACJA PRZEZ POTOKI (I/O) ---
 * int read_msg(int fd, int *val)      - Czyta z potoku ciąg znaków (do '\n') i zamienia na int.
 * int write_msg(int fd, int val)      - Zapisuje wartość int jako tekst z '\n' do potoku.
 * int write_byte(int fd, int val)     - Zapisuje dokładnie 1 bajt (rzutowany z int) do potoku.
 * int read_byte(int fd, int *val)     - Czyta dokładnie 1 bajt z potoku i zapisuje jako int.
 * void set_nonblock(int fd)           - Ustawia deskryptor w tryb nieblokujący (O_NONBLOCK).
 * * --- ZARZĄDZANIE POTOKAMI (DESKRYPTORY) ---
 * void close_pipe_ends(...)           - Zamyka wybrany koniec (0 lub 1) w tablicy potoków, 
 * z możliwością pominięcia jednego indeksu (exclude_idx).
 * void close_all_pipes(...)           - Zamyka absolutnie wszystkie końcówki w tablicy potoków.
 *
 * --- ZARZĄDZANIE PROCESAMI ---
 * void wait_for_all_children()        - Czeka (blokująco) aż wszystkie procesy potomne zginą.
 * void create_children_single_group() - Generuje 'n' procesów potomnych połączonych potokami
 * w architekturze "każdy z każdym".
 *
 * --- OBSŁUGA PLIKÓW ---
 * void read_file_line_by_line(...)    - Bezpiecznie otwiera plik, czyta go linia po linii 
 * za pomocą `getline` i parsuje używając `sscanf`.
 * ===================================================================================== */
int read_msg(int fd, int *val) {
    char buf[64];
    int i = 0;
    char c;
    // Zabezpieczamy read przed przerwaniem przez sygnał
    while (TEMP_FAILURE_RETRY(read(fd, &c, 1)) == 1) {
        if (c == '\n') break;
        if (i < 63) buf[i++] = c;
    }
    buf[i] = '\0';
    if (i == 0) return 0; // Łącze zostało zerwane (EOF)
    *val = atoi(buf);
    return 1;
}

// Pomocnicza funkcja do wysyłania liczby jako tekst
int write_msg(int fd, int val) {
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%d\n", val);
    // Zabezpieczamy write
    if (TEMP_FAILURE_RETRY(write(fd, buf, len)) < 0) {
        return -1; // Błąd zapisu (np. zerwane łącze)
    }
    return 0;
}

int write_byte(int fd, int val) {
    unsigned char c = (unsigned char)val;
    // Zabezpieczamy write
    return TEMP_FAILURE_RETRY(write(fd, &c, 1));
}

// Odczytuje jeden bajt z łącza i zapisuje jako int pod wskaźnik val
int read_byte(int fd, int *val) {
    unsigned char c;
    // Zabezpieczamy read
    int status = TEMP_FAILURE_RETRY(read(fd, &c, 1));
    if (status > 0) {
        *val = (int)c; // Rzutujemy bezpiecznie wczytany bajt na int
    }
    return status;
}
void wait_for_all_children() {
    pid_t pid;
    while (1) {
        pid = waitpid(-1, NULL, 0); // Czeka blokująco na JAKIEKOLWIEK dziecko
        if (pid < 0) {
            if (errno == ECHILD) {
                break; // ECHILD oznacza "nie ma już żadnych procesów potomnych"
            }
            if (errno == EINTR) {
                continue; // Przerwane przez sygnał, wznów czekanie
            }
            ERR("waitpid");
        }
    }
}
void create_pipes(int n, int* read_fds, int* write_fds) {
    int pipefd[2];
    for (int i = 0; i < n; i++) {
        if (pipe(pipefd) < 0) ERR("pipe");
        read_fds[i]  = pipefd[0];
        write_fds[i] = pipefd[1];
    }
}
// Funkcja zamyka określoną końcówkę (0 - read, 1 - write) w tablicy potoków
// Wymaga zdefiniowania: enum End { READ = 0, WRITE = 1 };
void close_pipe_ends(int pipes[][2], int n, int end_to_close, int exclude_idx) {
    for (int i = 0; i < n; i++) {
        if (i == exclude_idx) continue; // Pomijamy zamykanie dla wybranego indeksu
        if(TEMP_FAILURE_RETRY(close(pipes[i][end_to_close])) < 0)
            ERR("close");
    }
}

// Zamyka wszystkie potoki w tablicy (np. dla rodzica)
void close_all_pipes(int pipes[][2], int n) {
    for (int i = 0; i < n; i++) {
        if(TEMP_FAILURE_RETRY(close(pipes[i][0])) < 0)
            ERR("close");
        if(TEMP_FAILURE_RETRY(close(pipes[i][1])) < 0)
            ERR("close");
    }
}
void close_all_pipes_array(int pipes[], int n) {
    for (int i = 0; i < n; i++) {
        if(TEMP_FAILURE_RETRY(close(pipes[i])) < 0)
            ERR("close");
    }
}
void close_pipe_array_ends(int pipes[], int n, int exclude_idx) {
    for (int i = 0; i < n; i++) {
        if(i == exclude_idx) continue; 
        if(TEMP_FAILURE_RETRY(close(pipes[i])) < 0)
            ERR("close");
    }
}
void create_children_single_group(int n) {
    int pipes[n][2];
    int* write_fds = (int*)malloc(n * sizeof(int));

    // 1. Tworzenie wszystkich potoków
    for(int i = 0; i < n; i++) {
        if(pipe(pipes[i]) < 0) ERR("pipe");
    }

    // 2. Tworzenie procesów potomnych
    for(int i = 0; i < n; i++) {
        switch(fork()) {
            case -1:
                ERR("fork");
            case 0: // --- KOD DZIECKA ---
                // Dziecko czyta tylko ze swojego potoku (pipes[i][0])
                // Zamykamy odczyt we wszystkich innych potokach
                close_pipe_ends(pipes, n, 0, i); 
                
                // Dziecko nie pisze do samego siebie (zamykamy własny zapis)
                TEMP_FAILURE_RETRY(close(pipes[i][1]));

                // Zbieramy deskryptory do zapisu (do innych)
                for(int j = 0; j < n; j++) {
                    write_fds[j] = pipes[j][1];
                }

                int my_read_fd = pipes[i][0];
                set_nonblock(my_read_fd); // Jeśli zadanie wymaga I/O nieblokującego

                // TU WYWOŁUJESZ FUNKCJĘ PRACY DZIECKA:
                // child_work(my_read_fd, write_fds, n, i);

                // --- Zamykanie po pracy ---
                TEMP_FAILURE_RETRY(close(my_read_fd));
                close_pipe_ends(pipes, n, 1, i); // zamyka zapis do innych
                free(write_fds);
                exit(EXIT_SUCCESS);
        }
    }

    // --- KOD RODZICA ---
    // Rodzic nie używa potoków, więc zamyka WSZYSTKIE końcówki
    close_all_pipes(pipes, n);
    free(write_fds);
}


void read_file_line_by_line(const char* filepath) {
    FILE* file = fopen(filepath, "r");
    if (file == NULL) ERR("fopen");

    char* line = NULL; // getline samo zaalokuje pamięć, jeśli line to NULL
    size_t len = 0;
    ssize_t read;

    // --- OPCJONALNIE: Odczyt pierwszej specjalnej linii (np. liczba n) ---
    if ((read = getline(&line, &len, file)) != -1) {
        int n;
        if (sscanf(line, "%d", &n) == 1) {
            printf("Liczba elementów do wczytania: %d\n", n);
        }
    }

    // --- ODCZYT POZOSTAŁYCH LINII ---
    while ((read = getline(&line, &len, file)) != -1) {
        char name[64];
        int hp, attack;
        
        // sscanf to potężne narzędzie. Zwraca liczbę poprawnie dopasowanych zmiennych.
        // %63s zabezpiecza przed przepełnieniem tablicy name (zostawia 1 znak na '\0')
        if (sscanf(line, "%63s %d %d", name, &hp, &attack) == 3) {
            // Dane wczytane poprawnie, możesz je zapisać do tablicy struktur
            printf("Wczytano: Imię=%s, HP=%d, Atak=%d\n", name, hp, attack);
        } else {
            // Obsługa błędnego formatu w linii
            fprintf(stderr, "Nieprawidłowy format linii: %s", line);
        }
    }

    // WAŻNE: Wyczyść pamięć zaalokowaną przez getline pod wskaźnikiem line!
    free(line);
    
    if (fclose(file) != 0) ERR("fclose");
}
void set_nonblock(int fd) {
    // 1. Pobierz aktualne flagi
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) {
        ERR("fcntl F_GETFL");
    }

    // 2. Ustaw flagi z powrotem, dodając O_NONBLOCK (bitowe OR)
    if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) {
        ERR("fcntl F_SETFL");
    }
}
/*FIFO*/
// #include <stdio.h>
// #include <stdlib.h>
// #include <unistd.h>
// #include <fcntl.h>
// #include <sys/stat.h>
// #include <sys/types.h>
// #include <string.h>
// #include <errno.h>
// #include <sys/wait.h>

// 1. Funkcja tworząca FIFO
// Zwraca 0 w przypadku sukcesu, -1 w przypadku błędu.
int init_fifo(const char *fifo_name) {
    // mkfifo tworzy plik kolejki. 0666 to uprawnienia (rw-rw-rw-)
    if (mkfifo(fifo_name, 0666) == -1) {
        // Jeśli błąd polega na tym, że FIFO już istnieje, to nie problem.
        // Każdy inny błąd (np. brak uprawnień) należy zgłosić.
        if (errno != EEXIST) {
            perror("Błąd: Nie udało się utworzyć FIFO");
            return -1;
        }
    }
    return 0;
}

// 2. Funkcja do zapisu do FIFO
// Zwraca 0 w przypadku sukcesu, -1 w przypadku błędu.
int write_to_fifo(const char *fifo_name, const void *data, size_t size) {
    // UWAGA: open() tutaj zablokuje proces, dopóki ktoś nie otworzy FIFO do odczytu!
    int fd = open(fifo_name, O_WRONLY);
    if (fd == -1) {
        perror("Błąd: Nie udało się otworzyć FIFO do zapisu");
        return -1;
    }

    // Zapisujemy dane. write() zwraca liczbę zapisanych bajtów.
    ssize_t bytes_written = write(fd, data, size);
    if (bytes_written == -1) {
        perror("Błąd: Nie udało się zapisać danych do FIFO");
        close(fd);
        return -1;
    }

    close(fd); // Zawsze zamykaj deskryptory plików!
    return 0;
}

// 3. Funkcja do odczytu z FIFO
// Zwraca liczbę odczytanych bajtów lub -1 w przypadku błędu.
ssize_t read_from_fifo(const char *fifo_name, void *buffer, size_t buffer_size) {
    // UWAGA: open() tutaj zablokuje proces, dopóki ktoś nie otworzy FIFO do zapisu!
    int fd = open(fifo_name, O_RDONLY);
    if (fd == -1) {
        perror("Błąd: Nie udało się otworzyć FIFO do odczytu");
        return -1;
    }

    // Odczytujemy dane do bufora.
    ssize_t bytes_read = read(fd, buffer, buffer_size);
    if (bytes_read == -1) {
        perror("Błąd: Nie udało się odczytać danych z FIFO");
    }

    close(fd);
    return bytes_read;
}
