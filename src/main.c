#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <termios.h>
#include <limits.h>
#include <dirent.h>

#define BUFFER 16

const char *builtin_cmds [] = {"exit", "echo", "type", "pwd", "cd"}; // array of pointers to litterals
const char special_chars[] = {'\"', '$', '\'', '\\'};
const char *terminal_to_file_commands[] = {">", "1>", "2>", ">>", "1>>", "2>>"};

int is_executable_file(const char *fullpath){
  struct stat st;
  if (stat(fullpath, &st) != 0){ return 0; }
  if (!S_ISREG(st.st_mode)) { return 0; }
  return access(fullpath, X_OK) == 0;
}

int find_executable_prefix_match( const char *prefix, const char *path_env, char *out_match, size_t out_size){
  if (!prefix || !*prefix || !path_env || !out_match || out_size == 0){ return 0; }
  size_t prefix_len = strlen(prefix);
  char *path_copy = strdup(path_env);
  if (!path_copy){
    return 0;
  }
  char *saveptr = NULL;
  char *dir = strtok_r(path_copy, ":", &saveptr);
  int match_count = 0;
  char first_match[NAME_MAX + 1];
  first_match[0] = '\0';
  while (dir != NULL){
    const char *use_dir;
    if (dir[0] == '\0'){
      use_dir = ".";
    } else {
      use_dir = dir;
    }
    DIR *dp = opendir(use_dir);
    if (dp){
      struct dirent *entry;
      while ( (entry = readdir(dp)) != NULL){
        const char *name = entry->d_name;
        if (strncmp(name, prefix, prefix_len) != 0){
          continue;
        }
        char fullpath[PATH_MAX];
        int n = snprintf(fullpath, sizeof(fullpath), "%s/%s", use_dir, name);
        if (n<0 || (size_t)n >= sizeof(fullpath)){
          continue;
        }
        if (!is_executable_file(fullpath)){
          continue;
        }
        if (match_count == 0){
          strncpy(first_match, name, sizeof(first_match)- 1);
          first_match[sizeof(first_match) - 1] = '\0';
          match_count = 1;
        } else {
          if (strcmp(first_match, name) != 0){
            match_count = 2;
            closedir(dp);
            free(path_copy);
            return 2;
          }
        }
      }
      closedir(dp);
    }
    dir = strtok_r(NULL, ":", &saveptr);
  }
  free(path_copy);
  if (match_count == 1){
    strncpy(out_match, first_match, out_size - 1);
    out_match[out_size - 1] = '\0';
    return 1;
  }
  return 0;
}

/* Child process functions*/
int setup_redirection(int mode, const char *path, int *saved_fd, int *target_fd) {
  int writing_type = O_TRUNC;
  if (!saved_fd || !target_fd) {
    return 0;
  }
  *saved_fd = -1;
  *target_fd = -1;
  if (mode == 0) {
      return 1;
  }
  if (!path) {
      return 0;
  }
  if (mode == 1) {
    *target_fd = STDOUT_FILENO;
  } else if (mode == 2) {
    *target_fd = STDERR_FILENO;
  } else if (mode == 3 || mode == 4){
    *target_fd = STDOUT_FILENO;
    writing_type = O_APPEND;
  } else if (mode == 5) {
    *target_fd = STDERR_FILENO;
    writing_type = O_APPEND;
  } else {
    return 0;
  }
  *saved_fd = dup(*target_fd);
  if (*saved_fd < 0) {
    return 0;
  }
  int fd = open(path, O_WRONLY | O_CREAT | writing_type, 0644);
  if (fd < 0) {
    close(*saved_fd);
    *saved_fd = -1;
    return 0;
  }
  if (dup2(fd, *target_fd) < 0) {
    close(fd);
    close(*saved_fd);
    *saved_fd = -1;
    return 0;
  }
  close(fd);
  return 1;
}

int restore_redirection(int *saved_fd, int target_fd) {
  if (!saved_fd || *saved_fd < 0) {
    return 1;
  }
  if (dup2(*saved_fd, target_fd) < 0) {
    close(*saved_fd);
    *saved_fd = -1;
    return 0;
  }
  close(*saved_fd);
  *saved_fd = -1;
  return 1;
}

int setup_child_redirection(int mode, const char *path) {
  int writing_type = O_TRUNC;
  if (mode == 0) {
      return 1;
  }
  int target_fd;
  if (mode == 1) {
    target_fd = STDOUT_FILENO;
  } else if (mode == 2) {
    target_fd = STDERR_FILENO;
  } else if (mode == 3 || mode == 4){
    target_fd = STDOUT_FILENO;
    writing_type = O_APPEND;
  } else if (mode == 5) {
    target_fd = STDERR_FILENO;
    writing_type = O_APPEND;
  } else {
    errno = EINVAL;
    return 0;
  }
  int fd = open(path, O_WRONLY | O_CREAT | writing_type, 0644);
  if (fd < 0) {
    return 0;
  }
  if (dup2(fd, target_fd) < 0) {
    close(fd);
    return 0;
  }
  close(fd);
  return 1;
}

/* Change in terminal behaviour functions*/
int enable_raw_mode(struct termios *original){
  if (original == NULL){
    return 0;
  }
  if (tcgetattr(STDIN_FILENO, original) == -1){
    return 0;
  }
  struct termios raw = *original;

  raw.c_lflag &= ~(ICANON | ECHO);
  raw.c_cc[VMIN] = 1;
  raw.c_cc[VTIME] = 0;

  if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1){
    return 0;
  }
  return 1;
}

int disable_raw_mode(const struct termios *original){
  if (original == NULL){
    return 0;
  }
  tcsetattr(STDIN_FILENO, TCSAFLUSH, original);
  return 1;
}

/* Management of user entry and tab completition */
int handle_tab(char *buf, size_t *len, size_t cap, const char **cmds, size_t cmd_count, const char *path_env){
  if (!buf || !len || *len == 0){
    return 0;
  }
  if (strchr(buf, ' ') != NULL){
    return 0;
  }
  const char *builtin_match = NULL;
  size_t builtin_matches = 0;
  for (size_t i = 0; i < cmd_count; i++){
    if (strncmp(cmds[i], buf, *len) == 0){
      builtin_match = cmds[i];
      builtin_matches++;
    }
  }
  char exec_match[NAME_MAX + 1];
  exec_match[0] = '\0';
  int exec_result = find_executable_prefix_match(buf, path_env, exec_match, sizeof(exec_match));

  const char *final_match = NULL;
  int total_matches = 0;
  if (builtin_matches == 1){
    final_match = builtin_match;
    total_matches += 1;
  } else if (builtin_matches > 1){
    total_matches += 2;
  }
  if (exec_result == 1) {
    final_match = exec_match;
    total_matches += 1;
  } else if (exec_result == 2) {
    total_matches += 2;
  }

  if (total_matches != 1 || !final_match){
    printf("\n$ %s\a", buf);
    fflush(stdout);
    return 0;
  }
  size_t match_len = strlen(final_match);
  if (match_len + 2 > cap){
    return 0;
  }
  memcpy(buf, final_match, match_len);
  buf[match_len] = ' ';
  buf[match_len + 1] = '\0';
  *len = match_len + 1;

  printf("\r$ %s", buf);
  fflush(stdout);
  return 1;
}

int read_command_line(char *buf, size_t cap, const char *path_env){
  if (buf == NULL || cap == 0){
    return -1;
  }
  size_t cmd_count = sizeof(builtin_cmds) / sizeof(builtin_cmds[0]);
  size_t len = 0;
  buf[0] = '\0';
  while(1){
    char c;
    ssize_t n = read(STDIN_FILENO, &c, 1);
    if (n <= 0){
      return -1;
    }
    if (c == '\n' || c == '\r'){
      buf[len] = '\0';
      write(STDOUT_FILENO, "\n", 1);
      return (int)len;
    }
    if (c == 127 || c == '\b'){
      if (len > 0){
        len --;
        buf[len] = '\0';
        write(STDOUT_FILENO, "\b \b", 3);
      }
      continue;
    }
    if (c == '\t') {
      handle_tab(buf, &len, cap, builtin_cmds, cmd_count, path_env);
      continue;
    }

    if (c >= 32 && c < 127){
      if (len + 1 < cap){
        buf[len++] = c;
        buf[len] = '\0';
        write(STDOUT_FILENO, &c, 1);
      }
      continue;
    }
  }
}

int has_print_to_file_command(char *command){
  /* This function is for understanding if the command is for printing data to an external file */
  if (command == NULL) { return 0;}
  if (strcmp(command, ">") == 0 || strcmp(command, "1>") == 0){ return 1;}
  if (strcmp(command, "2>") == 0){ return 2; }
  if (strcmp(command, ">>") == 0){ return 3; }
  if (strcmp(command, "1>>") == 0){ return 4; }
  if (strcmp(command, "2>>") == 0){ return 5; }
  return 0;
}

int is_special_char(char input){
  /* This functon is for undrstnading if the char is part of a special character */
  size_t size = sizeof(special_chars) / sizeof(special_chars[0]);
  for (size_t i = 0; i < size; i++){
    if (input == special_chars[i]) return 1;
  }
  return 0;
}

size_t is_builtin_cmd(char *input){
  /* This function is for understanding if the command passed is a builtin command */
  size_t n = sizeof(builtin_cmds) / sizeof(builtin_cmds[0]);
  for(size_t aux = 0; aux < n; aux++ ){
    if (strcmp(input,builtin_cmds[aux]) ==0 ) return 1;
  }
  return 0;
}

static void free_argv(char **argv, int argc) {
  /* Freeing memory given a pointer of pointers */
  if (!argv) return;
  for (int i = 0; argv[i] != NULL; i++) {
    free(argv[i]);
    argv[i] = NULL;
  }
}

int parse_user_input(const char *input, char **argv, size_t argc_cap) {
  /* Function for tokenizing the user input into multiple tokens */
  if (!input || !argv || argc_cap == 0) return 0;
  size_t argc = 0;
  size_t i = 0;
  while (input[i] != '\0') {
    while (isspace((unsigned char)input[i])) i++;
    if (input[i] == '\0') break;
    if (argc + 1 >= argc_cap) break; // keep space for final NULL
    size_t cap = strlen(input);
    char *tok = (char *)malloc(cap);
    if (!tok) {
      free_argv(argv, (int)argc);
      return -2;
    }
    int in_squote = 0;
    int in_dquote = 0;
    size_t w = 0;
    while (input[i] != '\0') {
      unsigned char c = (unsigned char)input[i];
      if (!in_squote && !in_dquote && isspace(c)) {
        break;
      }
      if (in_dquote && c == '\\' && is_special_char(input[i+1]) && input[i + 1] != '\0'){
        tok[w++] = (unsigned char)input[++i];
        i++;
        continue;
      }
      if (c == '\\' && !in_dquote && !in_squote){
        tok[w++] = (unsigned char)input[++i];
        i++;
        continue;
      }
      if (c == '\'' && !in_dquote) {
        in_squote = !in_squote;
        i++;
        continue;
      }
      if (c == '\"' && !in_squote) {
        in_dquote = !in_dquote;
        i++;
        continue;
      }
      tok[w++] = (char)c;
      i++;
    }
    if (in_squote || in_dquote) {
      free(tok);
      free_argv(argv, (int)argc);
      return -1; /* unclosed quote */
    }
    tok[w] = '\0';
    char *shrunk = (char *)realloc(tok, w + 1); // Adjusting memory for the sampled token
    if (shrunk) tok = shrunk;
    argv[argc++] = tok;
  }
  argv[argc] = NULL;
  return (int)argc;
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
  /* If path is null */
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

/* Main Function */
int main(){
  struct termios original_termios;

  char user_input[4096];
  // size_t line_cap = 0; /* Temporary removed caused no longer using dynamic memory allocation (for semplicity) */
  char candidate[4096];
  char *argv[BUFFER]; // This is for the arguments
  int print_to_file = 0;
  int saved_fd = -1;
  int target_fd = -1;
  int special_command_position = 0;
  char *print_to_file_path;
  char *print_to_file_buffer;

  // Flush after every printf
  setbuf(stdout, NULL);
  const char *path_env = getenv("PATH");
  int path_exist = (path_env != NULL);
  if (!path_exist){
    printf("PATH does not exist or is corrupted.\n");
  }

  if (enable_raw_mode(&original_termios) == -1) {
    perror("enable_raw_mode");
    return 1;
  }

  while (1){
    print_to_file = 0;
    saved_fd = -1;
    target_fd = -1;
    special_command_position = -1;
    printf("$ ");
    // The function below allows a full string to be read, including the ending caracter '\n'
    int nread = read_command_line(user_input, sizeof(user_input), path_env);
    if (nread < 0){
      printf("\n");
      break; //<-- #TODO: this might need to be continue
    }

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
      free_argv(argv, BUFFER);
      continue;
    }
    
    /* The following check shoul be ideally done in the funciton that parses the tokens.
    However, for now, in order to keep modularity and division of concern, we keep it here.
    Personally, we are duplicating calculations in serching for something that is already 
    "serched" before. */
    for (size_t i = 0; i < argc && !print_to_file; i++){
      print_to_file = has_print_to_file_command(argv[i]);
      if (print_to_file){
        print_to_file_path = argv[i+1];
        special_command_position = i;
        continue;
      }
    }
    char *command = argv[0];
    if (strcmp(command, "exit") == 0){
      free_argv(argv, BUFFER);
      break;
    }
    /* Checking if it is a buit in command */
    if (is_builtin_cmd(command)){
      if (!setup_redirection(print_to_file, print_to_file_path, &saved_fd, &target_fd)) {
        perror("setup_redirection");
        free_argv(argv, BUFFER);
        continue;
      }
    }
    if (strcmp(command, "echo") == 0){
      if (print_to_file){
        argc = argc - 2;  // This should be done automatically and not manually like this
      }
      for (size_t i = 1; i < argc; i++){
        if (i > 1) printf(" ");
        printf ("%s", argv[i]);
      }
      printf("\n");
      restore_redirection(&saved_fd, target_fd);
      free_argv(argv, BUFFER);
      continue;
    }

    if (strcmp(command, "pwd") == 0){
      char *cwd = getcwd(NULL, 0); // Buffer and size automatically managed by UNIX
      // In this case we stored into cwd for later freeing up memory, cause internally
      // getcwd uses malloc. if we do not track it with a pointer, we lose track of memory
      if (!cwd){
        perror("getcwd");
        restore_redirection(&saved_fd, target_fd);
        free_argv(argv, BUFFER);
        continue;
      }
      printf("%s\n", cwd);
      restore_redirection(&saved_fd, target_fd);
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
        restore_redirection(&saved_fd, target_fd);
        free_argv(argv, BUFFER);
        continue;
      }
      if (chdir(dest) != 0){
        printf("cd: %s: No such file or directory\n",dest);
      }
      restore_redirection(&saved_fd, target_fd);
      free_argv(argv, BUFFER);
      continue;
    }

    if(strcmp(command, "type") == 0){
      if (argc < 2){
        printf("No command has been inserted.\n");
        restore_redirection(&saved_fd, target_fd);
        free_argv(argv, BUFFER);
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
      restore_redirection(&saved_fd, target_fd);
      free_argv(argv, BUFFER);
      continue;
    }

    if (path_exist && find_in_path(command, path_env, candidate, sizeof(candidate))){
      // Executing the file
      // printf("%s is an executable!\n",command);

      // I feel this section should be improved for better error handling and less bespoke code
      pid_t pid = fork(); // gets child PID
      if (pid == 0) { // we are in the child
        // TODO: make the if for saving to file or not with null in pointer >
        if (print_to_file){
          if (!setup_child_redirection(print_to_file, print_to_file_path)) {
            perror("redirection");
            _exit(1);
          }
          argv[special_command_position] = NULL;
        }
        execv(candidate, argv);
        perror("execv did not work out");
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
  // free(user_input);
  disable_raw_mode(&original_termios);
  return 0;
}
