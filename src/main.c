#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <errno.h>

const char *builtin_cmds [] = {"exit", "echo", "type", "pwd"}; // array of pointers to litterals

size_t is_builtin_cmd(char *input){
  size_t n = sizeof(builtin_cmds) / sizeof(builtin_cmds[0]);
  for(size_t aux = 0; aux < n; aux++ ){
    if (strcmp(input,builtin_cmds[aux]) ==0 ) return 1;
  }
  return 0;
}

size_t check_command(char *input, char *command) {
  // Check if the input starts with the command followed by a space
  int command_len = strlen(command);
  return strncmp(input, command, command_len) == 0 &&
         (input[command_len] == ' ' || input[command_len] == '\0');
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

size_t create_args_from_string(char *buf, const char *delims, char **argv, size_t argv_cap ){
  size_t argc = 0;
  char *save = NULL;

  char *tok = strtok_r(buf, delims, &save);
  while (tok != NULL){
    if (argc + 1 >= argv_cap) {
        break;
    }
    argv[argc] = tok;
    argc = argc + 1;
    // Pass NULL to continue from where it left off
    tok = strtok_r(NULL, delims, &save);
  }
  argv[argc] = NULL;
  return argc;
}



int main(){
  char user_input[50];
  char temp[50];
  const char *path = "PATH";           // This is the PATH varibale we want to look into the system
  int command_found = 0;
  char *command = NULL;
  char *path_command = NULL;
  char candidate[4096];
  char *argv[16]; // This is for the arguments

  // Flush after every printf
  setbuf(stdout, NULL);
  const char *path_env = getenv(path);
  int path_exist = (path_env != NULL);

  if (!path_exist){
    printf("PATH does not exist or is corrupted.\n");
  }

  while (1){
    command_found = 0;
    printf("$ ");
    // The function below allows a full string to be read, including the ending caracter '\n'
    if (fgets(user_input, sizeof(user_input), stdin) == NULL){
      printf("Null entry. Exiting...\n");
      return 0;
    }

    // With the below function, what we are doing is calculating the size of the string,
    // stopping at the character '\n'
    user_input[strcspn(user_input, "\n")] = '\0';
    // Isolating the first part of the string, which is meant to be a command

    strncpy(temp, user_input, sizeof(user_input));
    temp[sizeof(temp) - 1] = '\0';
    command = strtok(temp, " ");
    
    if ( !command_found && (command != NULL) && check_command(command, "exit")){
      return 0;
    }

    if ( !command_found && (command != NULL) && check_command(command, "echo")){
      command = strtok(NULL, " ");
      while(command) { 
        printf("%s ",command);
        command = strtok(NULL, " ");
      }
      command_found = 1;
      printf("\n");
    }

    if (!command_found && (command != NULL) && check_command(command, "pwd")){
      printf("%s\n", getcwd(NULL, 0)); // Buffer and size automatically managed by UNIX
      command_found = 1;
    }

    if(!command_found && (command != NULL) && check_command(command, "type")){
      command = strtok(NULL, " ");
      if (command ==NULL){
        printf("No command has been inserted.\n");
        continue;
      }
      if (is_builtin_cmd(command)){
        printf("%s is a shell builtin\n", command);
        command_found = 1;
      } 
      else {
        if (path_exist && find_in_path(command, path_env, candidate, sizeof(candidate))) {
          printf("%s is %s\n", command, candidate);
          command_found = 1;
        } 
        else {
          printf("%s: not found\n", command);
          command_found = 1;
        }
      }
    }
    else if (!command_found && path_exist && find_in_path(command, path_env, candidate, sizeof(candidate))){
      // Executing the file
      // printf("%s is an executable!\n",command);
      size_t argc = create_args_from_string(user_input, " \t\r\n", argv, 16);

      // I feel this section should be improved for better error handling and less bespoke code
      pid_t pid = fork(); // gets child PID
      if (pid == 0) { // we are in the child
          execv(candidate, argv);
          perror("execv did not work out.");   // only if execv fails
          _exit(127);
      } else if (pid > 0) { // We are in the parent
          int status;
          waitpid(pid, &status, 0);
          command_found = 1;
      } else {
          perror("Fork did not work out.");
      }
    }
    if (!command_found){
      printf("%s: command not found\n", command);
    }
  }
  return 0;
}
