#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>
#include <ctype.h>

#define BUFFER 16

const char *builtin_cmds [] = {"exit", "echo", "type", "pwd", "cd"}; // array of pointers to litterals

static void free_argv(char **argv, int argc) {
    if (!argv) return;
    for (int i = 0; argv[i] != NULL; i++) {
        free(argv[i]);
        argv[i] = NULL;
    }
}

int parse_user_input(const char *input, char **argv, size_t argc_cap) {
  size_t argc = 0;
  size_t i = 0;
  int in_squote = 0;
  int in_dquote = 0;

  if (!input || !argv || argc_cap == 0) return 0;
  while (input[i] != '\0') {
    while (isspace((unsigned char)input[i])) i++; // we do unsigned casting cause isspace does not like signed char
    if (input[i] == '\0') break;
    if (argc + 1 >= argc_cap) break;  // keep space for final NULL

    size_t start = i;
    size_t out_len = 0;

    // First pass: find end of token and compute output length excluding quotes
    in_squote = 0;
    in_dquote = 0;
    while (input[i] != '\0') {
      unsigned char c = (unsigned char)input[i];
      if (!in_squote && !in_dquote && isspace(c)) break;
      if (c == '\'' && !in_dquote) {
        in_squote = !in_squote;
        i++;
        continue;
      }
      if (c == '\"' && !in_squote){
        in_dquote = !in_dquote;
        i++;
        continue;
      }
      out_len++;
      i++;
    }
    if (in_squote || in_dquote) {  // quote not closed before token ended
      free_argv(argv, (int)argc);
      return -1;
    }
    char *tok = (char *)malloc(out_len + 1);
    if (!tok) {
      free_argv(argv, (int)argc);
      return -2;
    }

    // Second pass: copy chars excluding quotes
    size_t r = start;
    size_t w = 0;
    in_squote = 0;
    in_dquote = 0;
    while (w < out_len && input[r] != '\0') {
      unsigned char c = (unsigned char)input[r];
      if (!in_squote && !in_dquote && isspace(c)) break;
      if (c == '\'' && !in_dquote) {
        in_squote = !in_squote;
        r++;
        continue;
      }
      if (c == '\"' && !in_squote){
        in_dquote = !in_dquote;
        r++;
        continue;
      }
      tok[w++] = (char)c;
      r++;
    }
    tok[w] = '\0';
    argv[argc++] = tok;
    // i is already at whitespace or '\0'. Next loop will skip spaces.
  }
  argv[argc] = NULL;
  return (int)argc;
}

size_t is_builtin_cmd(char *input){
  size_t n = sizeof(builtin_cmds) / sizeof(builtin_cmds[0]);
  for(size_t aux = 0; aux < n; aux++ ){
    if (strcmp(input,builtin_cmds[aux]) ==0 ) return 1;
  }
  return 0;
}

int find_in_path(const char *command, const char *path_env, char *out, size_t out_len){
  // remember that *command is the first letter of the string.
  // we are checking that it is not empty with '\0'
  if (!command ||!*command || !out || out_len == 0){
    return 0;
  }

  // If the user is giving a path starting with /, I go and check the executable directly
  if (strchr(command, '/')){
    int n = snprintf(out, out_len, "%s", command);
    if (n < 0 || (size_t)n >= out_len){
      return 0;
    }
    return access(out, X_OK) == 0;
  }

  // If path is null
  if(!path_env){
    return 0;
  }

  size_t len = strlen(path_env); // size_t is an unsigned integer. int is signed integer
  char path_copy[len + 1];
  memcpy(path_copy, path_env, len + 1); // memcpy(*to, *from, numBytes);

  char *saveptr = NULL;
  char *dir = strtok_r(path_copy, ":", &saveptr);
  while( dir != NULL){
    const char *use_dir;
    // below options for double :: (should they be there)
    if (dir[0] == '\0'){
      use_dir = ".";
    }else{
      use_dir = dir;
    }
    // end optional
    int n = snprintf(out, out_len, "%s/%s", use_dir, command);
    if (n > 0 && (size_t)n < out_len){
      if(access(out, X_OK) == 0){ 
        return 1;
      }
    }
    dir = strtok_r(NULL, ":", &saveptr);
  }
  return 0;
}

int main(){
  char *user_input = NULL;
  size_t line_cap = 0;
  char candidate[4096];
  char *argv[BUFFER]; // This is for the arguments

  // Flush after every printf
  setbuf(stdout, NULL);
  const char *path_env = getenv("PATH");
  int path_exist = (path_env != NULL);
  if (!path_exist){
    printf("PATH does not exist or is corrupted.\n");
  }
  while (1){
    printf("$ ");
    ssize_t nread = getline(&user_input, &line_cap, stdin); // POSIX, hence ok for macOS and Linux. Hides several calls for memory management
    // The function below allows a full string to be read, including the ending caracter '\n'
    if (nread < 0){
      printf("\n");
      break; //<-- #TODO: this might need to be continue
    }

    // With the below function, what we are doing is calculating the size of the string,
    // stopping at the character '\n'
    user_input[strcspn(user_input, "\n")] = '\0';
    // Isolating the first part of the string, which is meant to be a command
    int argc = parse_user_input(user_input, argv, BUFFER);
    if (argc == -1){
      printf("Sintax error: unterminated single quote\n");
      free_argv(argv, BUFFER);
      continue;
    }
    if (argc == 0) {
      free_argv(argv, BUFFER);
      continue;
    }
    if (argv[0] == NULL) {
      continue;
    }
    char *command = argv[0];
    if (strcmp(command, "exit") == 0){
      free_argv(argv, BUFFER);
      break;
    }

    if (strcmp(command, "echo") == 0){
      for (size_t i = 1; i < argc; i++){
        if (i > 1) printf(" ");
        printf ("%s", argv[i]);
      }
      printf("\n");
      free_argv(argv, BUFFER);
      continue;
    }

    if (strcmp(command, "pwd") == 0){
      char *cwd = getcwd(NULL, 0); // Buffer and size automatically managed by UNIX
      // In this case we stored into cwd for later freeing up memory, cause internally
      // getcwd uses malloc. if we do not track it with a pointer, we lose track of memory
      if (!cwd){
        perror("getcwd");
        free_argv(argv, BUFFER);
        continue;
      }
      printf("%s\n", cwd);
      free(cwd);
      free_argv(argv, BUFFER);
      continue;
    }

    if (strcmp(command, "cd") == 0){
      const char *dest = NULL;
      if (argc >= 2){
        dest = argv[1];
        if (strcmp(dest, "~") == 0){
          dest = getenv("HOME");
        }
      }else{
        dest = getenv("HOME");
      }

      if (!dest) {
        printf("cd: HOME not set\n");
        continue;
      }
      if (chdir(dest) != 0){
        printf("cd: %s: No such file or directory\n",dest);
      }
      continue;
    }

    if(strcmp(command, "type") == 0){
      if (argc < 2){
        printf("No command has been inserted.\n");
        continue;
      }
      char *q = argv[1];
      if (is_builtin_cmd(q)){
        printf("%s is a shell builtin\n", q);
      } else if (path_exist && find_in_path(q, path_env, candidate, sizeof(candidate))) {
          printf("%s is %s\n", q, candidate);
      } else {
          printf("%s: not found\n", q);
      }
      free_argv(argv, BUFFER);
      continue;
    }

    if (path_exist && find_in_path(command, path_env, candidate, sizeof(candidate))){
      // Executing the file
      // printf("%s is an executable!\n",command);

      // I feel this section should be improved for better error handling and less bespoke code
      pid_t pid = fork(); // gets child PID
      if (pid == 0) { // we are in the child
          execv(candidate, argv);
          perror("execv did not work out.");   // only if execv fails
          _exit(127);
      } else if (pid > 0) { // We are in the parent
          int status;
          waitpid(pid, &status, 0);
      } else {
          perror("Fork did not work out.");
      }
      free_argv(argv, BUFFER);
      continue;
    }
    printf("%s: command not found\n", command);
  }
  free(user_input);
  return 0;
}
