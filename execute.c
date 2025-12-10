#include <unistd.h>
#include "parse.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#define MAX_DIR_STACK 100
static char *dir_stack[MAX_DIR_STACK];
static int dir_stack_top = -1;

void run_pipeline(struct pipeline *p) {
// p - вся строка. типа "echo "hello world" | wc -c"
    struct command *cmd = &p->first_command; // первая комманда echo "hello world"
    int pipefd[2]; // пайп, 0 - читать 1 - писать
    int prev_pipe = -1; // пайп от предыдущей команды
    pid_t *pids = NULL; // массив PID ов
    int num_commands = 0;
    
    struct command *tmp = cmd;
    while (tmp && tmp->argc > 0) { // проверяем что команда не пустая
        num_commands++; // число команд + 1
        tmp = tmp->next; // идем к следующей
    }
    
    if (num_commands == 0) return; // если команд нет - выходим
    
    pids = malloc(num_commands * sizeof(pid_t)); // выделяем память под ПИД
    if (!pids) {
        perror("malloc");
        return;
    }
    
    int i = 0;
    while (cmd && cmd->argc > 0) { // пока есть команды
        if (cmd->next && cmd->next->argc > 0) { // надо ли создавать трубу? есть ли следующая команда?
            if (pipe(pipefd) < 0) { // если нет - выходим
                perror("pipe");
                free(pids);
                return;
            }
        }
        
        pid_t pid = fork(); // создаем ребенка
        if (pid == 0) { // в ребенке пид == 0
            if (prev_pipe != -1) {
                dup2(prev_pipe, STDIN_FILENO); // подключаем ввод из предыдущей трубы
                close(prev_pipe);
            }
            
            if (cmd->next && cmd->next->argc > 0) { // если есть следующая команда
                close(pipefd[0]); // закрываем на чтение
                dup2(pipefd[1], STDOUT_FILENO); // делаем вывод в следующую трубу
                close(pipefd[1]); // закываем
            }
            
            if (cmd->input_redir) { // если есть файл
                int fd = open(cmd->input_redir, O_RDONLY); // открываем для чтения
                if (fd < 0) { // если не получилось открыть
                    fprintf(stderr, "Cannot open %s: %s\n", cmd->input_redir, strerror(errno));
                    exit(1);
                }
                dup2(fd, STDIN_FILENO); // заменяем ввод на файл fd
                close(fd);
            }
            
            if (cmd->output_redir) { // если есть файл
                int fd = open(cmd->output_redir, O_WRONLY | O_CREAT | O_TRUNC, 0644);
                // O_WRONLY - открыть для записи
                // O_CREAT - создать если нет
                // O_TRUNC - очистить если есть
                // 0644 - права: владелец(rw), остальные(r)
                if (fd < 0) {
                    fprintf(stderr, "Cannot create %s: %s\n", cmd->output_redir, strerror(errno)); // если не открылся или не создался
                    exit(1);
                }
                dup2(fd, STDOUT_FILENO); // заменяем вывод на файл fd
                close(fd);
            }
            
            execvp(cmd->argv[0], cmd->argv);
            //execvp("ls", ["ls", "-la", NULL]);
            // система ищет программу "ls" в PATH
            // находит /bin/ls
            // выгружает  код shell из памяти
            // зщагружает код /bin/ls в ту же память
            // начинает выполнять код ls
            fprintf(stderr, "Command not found: %s\n", cmd->argv[0]); // если программа не найдена выводимо шибку
            exit(127); // код ошибки команда не найдена
        } else if (pid > 0) { // форк вернул пид ребенка
            pids[i++] = pid; //записываем пид в массив пидов
            
            if (prev_pipe != -1) {
                close(prev_pipe); // закрываем трубу от предыдущей команды (если есть)
            }
            
            if (cmd->next && cmd->next->argc > 0) { // если есть следующая команда
                close(pipefd[1]); // закрываем запись
                prev_pipe = pipefd[0]; // сохраняем чтение
            }
        } else {
            perror("fork"); // если нельзя создать новый процесс выводим ошибку освобождаем память и выходим
            free(pids);
            return;
        }
        
        cmd = cmd->next; // переходим к следующей команде в цепочке
    }
    
    if (prev_pipe != -1) { // после цикла закрываем трубу последней команды
        close(prev_pipe);
    }
    
    if (!p->background) { // если это НЕ фоновая задача
        for (int j = 0; j < num_commands; j++) {
            waitpid(pids[j], NULL, 0); // ждем пока завершится каждый ребенок, 0 - обычное ожидание
        }
    }
    
    free(pids); // освобождаем память
}

void run_builtin(enum builtin_type builtin, char *builtin_arg) {
    switch (builtin) {
        case BUILTIN_EXIT: {
            int code = 0; // код выхода по умолчанию 0 - ВСЕ ХОРОШО
            if (builtin_arg) { // если юзер передал код
                code = atoi(builtin_arg); // меняем строку на чилсо и записываем в переменную код
            }
            exit(code); // выходим с определенным кодом
            break;
        }
        
        case BUILTIN_WAIT: {
            if (builtin_arg) { // если юзер передал пид
                pid_t pid = atoi(builtin_arg); // присваиваем пид переменной пид
                waitpid(pid, NULL, 0); // делаем вейт пид с определенным юзером пидом
            } else { // если не передал
                waitpid(-1, NULL, 0); // ждем любого ребенка
            }
            break;
        }
        
        case BUILTIN_KILL: {
            if (!builtin_arg) {
                fprintf(stderr, "kill: missing PID\n");
                return;
            }
            
            char *endptr;
            long pid = strtol(builtin_arg, &endptr, 10); // пытаемся конвертировать строку в число
            if (endptr == builtin_arg || *endptr != '\0' || pid <= 0) { //  проверка что strtol не прочитал ни однрого символа, нет мусора после числа
                fprintf(stderr, "kill: invalid PID: %s\n", builtin_arg); // и что PID > 0. и если нет - выводим ошибку
                return;
            }
            
            if (kill((pid_t)pid, SIGTERM) < 0) { // отпарвляем сигнал и делаем вывод в зависимости от ошибки
                if (errno == ESRCH) {
                    fprintf(stderr, "kill: No such process\n");
                } else if (errno == EPERM) {
                    fprintf(stderr, "kill: Permission denied\n");
                } else {
                    perror("kill");
                }
            }
            break;
        }
                
        case BUILTIN_DECLARE: {
            if (!builtin_arg) { // если аргументов нет
                extern char **environ; // массив переменных окружения
                for (char **env = environ; *env; env++) {
                    printf("%s\n", *env); // просто выводим все переменные
                }
            } else {
                char *equals = strchr(builtin_arg, '=');// ищем =
                if (equals) { // если есть
                    *equals = '\0'; //делим строку на 2 части
                    char *name = builtin_arg;// часть до =
                    char *value = equals + 1; // часть после =
                    setenv(name, value, 1);// часть до = равна части после =. отлично
                } else { // если = нет 
                    unsetenv(builtin_arg); // удаляем переменную
                }
            }
            break;
        }
        
        case BUILTIN_CD: {
            char *path = builtin_arg;
            if (!path) {
                path = getenv("HOME");
                if (!path) {
                    fprintf(stderr, "cd: HOME not set\n");
                    return;
                }
            }
            
            if (chdir(path) < 0) {
                fprintf(stderr, "cd: %s: %s\n", path, strerror(errno));
            } else {
                char cwd[1024];
                if (getcwd(cwd, sizeof(cwd))) {
                    setenv("PWD", cwd, 1);
                }
            }
            break;
        }
        
        case BUILTIN_PUSHD: {
            if (!builtin_arg) {
                fprintf(stderr, "pushd: missing directory\n");
                return;
            }
            
            char cwd[1024];
            if (!getcwd(cwd, sizeof(cwd))) {
                perror("pushd: getcwd");
                return;
            }
            
            if (dir_stack_top >= MAX_DIR_STACK - 1) {
                fprintf(stderr, "pushd: directory stack full\n");
                return;
            }
            
            dir_stack[++dir_stack_top] = strdup(cwd);
            
            if (chdir(builtin_arg) < 0) {
                fprintf(stderr, "pushd: %s: %s\n", builtin_arg, strerror(errno));
                free(dir_stack[dir_stack_top]);
                dir_stack_top--;
                return;
            }
            
            if (getcwd(cwd, sizeof(cwd))) {
                setenv("PWD", cwd, 1);
            }
            break;
        }
        
        case BUILTIN_POPD: {
            if (dir_stack_top < 0) {
                fprintf(stderr, "popd: directory stack empty\n");
                return;
            }
            
            char *prev_dir = dir_stack[dir_stack_top--];
            if (chdir(prev_dir) < 0) {
                fprintf(stderr, "popd: %s: %s\n", prev_dir, strerror(errno));
            } else {
                char cwd[1024];
                if (getcwd(cwd, sizeof(cwd))) {
                    setenv("PWD", cwd, 1);
                }
            }
            free(prev_dir);
            break;
        }
        
        case BUILTIN_HELP: {
            if (!builtin_arg) {
                printf("Simple Shell - Available builtins:\n");
                printf("  exit [code]    - exit shell with optional code\n");
                printf("  wait [pid]     - wait for process\n");
                printf("  kill pid       - send SIGTERM to process\n");
                printf("  declare [var]  - show/set environment variables\n");
                printf("  cd [dir]       - change directory\n");
                printf("  pushd dir      - push directory to stack\n");
                printf("  popd           - pop directory from stack\n");
                printf("  help [cmd]     - show this help\n");
            } else {
                if (strcmp(builtin_arg, "exit") == 0) {
                    printf("exit [CODE] - Exit shell with optional code\n");
                } else if (strcmp(builtin_arg, "wait") == 0) {
                    printf("wait [PID] - Wait for process with optional PID\n");
                } else if (strcmp(builtin_arg, "kill") == 0) {
                    printf("kill PID - Send SIGTERM to process\n");
                } else if (strcmp(builtin_arg, "declare") == 0) {
                    printf("declare [NAME[=VALUE]] - Show/set environment variables\n");
                } else if (strcmp(builtin_arg, "cd") == 0) {
                    printf("cd [DIRECTORY] - Change current directory\n");
                } else if (strcmp(builtin_arg, "pushd") == 0) {
                    printf("pushd DIRECTORY - Push directory to stack and change to it\n");
                } else if (strcmp(builtin_arg, "popd") == 0) {
                    printf("popd - Pop directory from stack and change to it\n");
                } else if (strcmp(builtin_arg, "help") == 0) {
                    printf("help [NAME] - Show help for builtin commands\n");
                } else {
                    fprintf(stderr, "help: no help topic for '%s'\n", builtin_arg);
                }
            }
            break;
        }
        
        default:
            fprintf(stderr, "Unknown builtin command\n");
    }
}
