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
#define SHELL_EXIT_REQUESTED 99
#define HISTORY_CAPACITY 16

typedef struct{
  char text[PATH_MAX];
  int is_dir;
} PathMatch;

/* The below two structs is for working with pipelines */
typedef struct {
  char **argv;
  int argc;
} Command;

typedef struct {
  Command *cmds;
  int count;
} Pipeline;
/* ------ */

typedef struct {
    char **entries;
    int count;
} History;

const char *builtin_cmds [] = {"exit", "echo", "type", "pwd", "cd", "history"}; // array of pointers to litterals
const char special_chars[] = {'\"', '$', '\'', '\\'};
const char *terminal_to_file_commands[] = {">", "1>", "2>", ">>", "1>>", "2>>"};

/* Helpers to manage ArrowUP and ArrowDOWN */
void redraw_input_line(const char *prompt, const char *buf, size_t old_len, size_t new_len){
  /*
  We are using write because we are n raw mode due to termios. The input is not longer
  line buffered, so we are controlling every character. In this scenario, printf (or similar)
  do not work becuase we are no longer in the stdio layer.
  */
  write(STDOUT_FILENO, "\r", 1);
  write(STDOUT_FILENO, prompt, strlen(prompt));
  write(STDOUT_FILENO, buf, new_len);
  if (old_len > new_len){
    size_t extra = old_len - new_len;
    for (size_t i = 0; i < extra; i++){
      write(STDOUT_FILENO, " ", 1);
    }
    for (size_t i = 0; i < extra; i++){
      write(STDOUT_FILENO, "\b", 1);
    }
  }
}

void replace_buffer(char *buf, size_t cap, size_t *len, const char *src){
  /* This function swap two buffers, namely puts src into buf. */
  if (!buf || !len || !src || cap ==0){
    return;
  }
  size_t n = strlen(src);
  if (n >= cap){
    n = cap - 1;
  }
  memcpy(buf, src, n);
  buf[n] = '\0';
  *len = n;
}

int handle_arrow_up(char *buf, size_t cap, size_t *len, History *h, int *h_index, int *nav_history, char *draft_buf, size_t draft_cap, size_t *cursor, const char *prompt){
  if (!buf || !len || !h || !h_index || !nav_history || !draft_buf || !prompt) {
    return -1;
  }
  if (h->count == 0){
    return -2;
  }
  size_t old_len = *len;
  if (!(*nav_history)){
    strncpy(draft_buf, buf, draft_cap - 1); /* strcpy does not guarantee NULL termination */
    draft_buf[draft_cap - 1] = '\0';
    *nav_history = 1;
    *h_index = h->count -1;
  } else if (*h_index > 0){
    (*h_index)--;
  }
  replace_buffer(buf,cap,len,h->entries[*h_index]);
  *cursor = *len;
  redraw_input_line(prompt, buf, old_len, *len);
  return 1;
}

int handle_arrow_down(char *buf, size_t cap, size_t *len, History *h, int *h_index, int *nav_history, char *draft_buf, size_t *cursor, const char *prompt){
  if (!buf || !len || !h || !h_index || !nav_history || !draft_buf || !prompt) {
    return -1;
  }
  if (!(*nav_history)) return -2;
  if (h->count == 0) return -3;
  size_t old_len = *len;
  if (*h_index < h->count - 1){
    (*h_index)++;
    replace_buffer(buf,cap,len,h->entries[*h_index]);
    *cursor = *len;
  } else{
    *h_index = h->count;
    *nav_history = 0;
    replace_buffer(buf,cap,len,draft_buf);
    *cursor = *len;
  }
  redraw_input_line(prompt, buf, old_len, *len);
  return 1;
}

/* Helpers for managing ArrowLEFT and ArrowRIGHT */
int handle_arrow_left(size_t *cursor){
  if (!cursor) return -1;
  if (*cursor == 0) return -2;
  (*cursor)--;
  if(write(STDOUT_FILENO, "\x1b[D", 3) < 0){ /* means error */
    return -3;
  }
  return 1;
}

int handle_arrow_right(size_t *cursor, int len){
  if (!cursor) return -1;
  if (*cursor >= len) return -2;
  (*cursor)++;
  if(write(STDOUT_FILENO, "\x1b[C", 3) < 0){
    return -3;
  }
  return 1;
}

/* Hlepers to manage the history */
void free_history(History *h){
  for (int i = 0; i < h->count; i++){
    free(h->entries[i]);
  }
  free(h->entries);
  h->entries = NULL;
  h->count = 0;
}

int save_history(History *h, const char *user_input){
  if (h->count >= HISTORY_CAPACITY){
    int capacity = HISTORY_CAPACITY * 2;
    char **tmp = realloc(h->entries, capacity * sizeof(char *));
    if (!tmp){
      perror("realloc");
      return -1;
    }
    h->entries = tmp;
  }
  size_t len = strlen(user_input) + 1;
  h->entries[h->count] = malloc(len);
  if (!h->entries[h->count]){
    perror("malloc");
    return 1;
  }
  memcpy(h->entries[h->count], user_input, len);
  h->count++;
  return 0;
}

void print_history(const History *h, char **argv){
  if (!h) {
    fprintf(stderr, "history: no history available\n");
    return;
  }
  int n = h->count;
  if (argv[1] != NULL) {
    char *end;
    long val = strtol(argv[1], &end, 10); /* <--- this is to avoid atoi() functon */
    if (*argv[1] == '\0' || *end != '\0' || val <= 0 || val > INT_MAX) {
      fprintf(stderr, "history: invalid argument\n");
      return;
    }
    n = (int)val;
    if (n <= 0){
      fprintf(stderr, "history: invalid argument\n");
      return;
    }
    if (n > h->count){
      n = h->count;
    }
  }
  int start = h->count - n;
  for (int i = start; i < h->count; i++){
    printf("\t%d %s\n", i+1, h->entries[i]);
  }
}

/* Helper to avoid that multiple /// are displayed when in reality we want just one / (try echo ./// and it will not give a problem).
I discovered that in UNIX system, the multiple // are not an issue, but for a shell it is better to avoid that display.      */
int normalise_trailing_slashes(char *path){
  if (!path){
    return 0;
  }
  size_t len = strlen(path);
  if (len == 0){
    return 0;
  }
  while (len > 1 && path[len-1] == '/' && path[len-2] == '/'){
    path[len - 1] = '\0';
    len--;
  }
  return 1;
}

/* Helper to find where a token start given the user input */
char *find_current_token_start(char *buf, size_t len){
  if(!buf){
    return NULL;
  }
  size_t i = len;
  while (i > 0 && buf[i-1] != ' '){
    i--;
  }
  return &buf[i];
}

/* Helper to split a token at the last / found in the string */
int split_path_token(const char *token, char *dir_to_open, size_t dir_cap, char *prefix, size_t prefix_cap, char *replacement_base, size_t base_cap){
  if (!token || !dir_to_open || !prefix || !replacement_base){
    return 0;
  }
  const char *last_slash = strrchr(token, '/'); /* Retunr the pointer to the last char, in this case / */
  if (!last_slash){
    if (snprintf(dir_to_open, dir_cap, "./") >= (int)dir_cap){
      return 0;
    }
    if (snprintf(prefix, prefix_cap, "%s", token) >= (int)prefix_cap){
      return 0;
    }
    replacement_base[0] = '\0';
    return 1;
  }
  size_t dir_len = (size_t) (last_slash - token + 1);
  if (dir_len + 1 > dir_cap || dir_len + 1 > base_cap){
    return 0;
  }
  memcpy(dir_to_open, token, dir_len);
  dir_to_open[dir_len] = '\0';
  memcpy(replacement_base, token, dir_len);
  replacement_base[dir_len] = '\0';
  /* Removing multiple final slashes // */
  if ( normalise_trailing_slashes(dir_to_open) == 0 || normalise_trailing_slashes(replacement_base) == 0){
    return 0;
  }
  /* END normalization */
  if (snprintf(prefix,prefix_cap, "%s", last_slash + 1) >= (int)prefix_cap){
    return 0;
  }
  return 1;
}

/* Helper to check if the path is a directory */
int path_is_directory(const char *path){
  if (!path){ return 0; }
  struct stat st;
  if (stat(path, &st) != 0) { return 0; }
  return S_ISDIR(st.st_mode);
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

/* Helper to scan a directory and collect matches */
size_t find_path_matches(const char *dir_to_open, const char *prefix, const char *replacement_base, PathMatch *matches, size_t max_matches){
  if (!dir_to_open || !prefix || !replacement_base || !matches || max_matches == 0){
    return 0;
  }
  DIR *dir = opendir(dir_to_open);
  if (!dir){
    return 0;
  }
  struct dirent *entry;
  size_t count = 0;
  size_t prefix_len = strlen(prefix);
  while ( (entry = readdir(dir)) != NULL){
    const char *name = entry ->d_name;
    if (strcmp(name, ".") == 0 || strcmp (name, "..") == 0){
      continue;
    }
    if (strncmp(name, prefix, prefix_len) != 0){
      continue;
    }
    if (count >= max_matches){
      break;
    }
    if (snprintf(matches[count].text, sizeof(matches[count].text), "%s%s", replacement_base, name) >= (int) sizeof(matches[count].text)){
      continue;
    }
    char full_fs_path[PATH_MAX];
    if (snprintf(full_fs_path, sizeof(full_fs_path), "%s%s", dir_to_open, name) >= (int)sizeof(full_fs_path)){
      continue;
    }
    matches[count].is_dir = path_is_directory(full_fs_path);
    // printf("DEBUG match='%s' full='%s' is_dir=%d\n",
    //    matches[count].text, full_fs_path, matches[count].is_dir);
    count++;
  }
  closedir(dir);
  return count;
}

/* Sorting function for printing the exec files and built in commands. Required for qsort. This is generated by AI-Copilot*/
int cmp_strings(const void *a, const void *b){
  const char *sa = (const char *)a;
  const char *sb = (const char *)b;
  return strcmp(sa, sb);
}

/* Sorting function for the struct PatchMatch. This is generated by AI-Copilot*/
int cmp_path_matches(const void *a, const void *b){
  const PathMatch *pa = (const PathMatch *) a;
  const PathMatch *pb = (const PathMatch *) b;
  return strcmp(pa->text, pb->text);
}

int is_executable_file(const char *fullpath){
  struct stat st;
  if (stat(fullpath, &st) != 0){ return 0; }
  if (!S_ISREG(st.st_mode)) { return 0; }
  return access(fullpath, X_OK) == 0;
}

/* Return the length of the longest common prefix shared by all strings */
size_t longest_common_len(char matches[][NAME_MAX + 1], size_t count){
  if (count == 0){
    return 0;
  }
  size_t i = 0;
  while (matches[0][i] != '\0'){
    char c = matches[0][i];
    for (size_t j = 0; j < count; j++){
      if (matches[j][i] != c){
        return i;
      }
    }
    i++;
  }
  return i;
}

/* Retunrs the lenght of the longest common prefix for PathMatch */
/* NOTE: some fuctionalities can be merged with the pre-existing funciton, but this will be done later */
size_t longest_common_len_path_matches(PathMatch *matches, size_t count){
  if (!matches || count == 0){
    return 0;
  }
  size_t lcp = strlen (matches[0].text);
  for (size_t i = 1; i < count; i++){
    size_t j = 0;
    while (j < lcp && matches[0].text[j] != '\0' && matches[i].text[j] != '\0' && matches[0].text[j] == matches[i].text[j]){
      j++;
    }
    lcp = j;
  }
  return lcp;
}

/* Helper to replace the current token inside the given buffer buf */
int replace_token_in_buffer(char *buf, size_t *len, size_t cap, size_t token_offset, size_t token_len, const char *replacement, int appent_extra_char, char extra_char){
  if (!buf || !len || ! replacement){
    return 0;
  }
  size_t replacement_len = strlen(replacement);
  size_t extra_len;
  if (appent_extra_char){
    extra_len = 1;
  } else {
    extra_len = 0;
  }
  size_t new_len = *len - token_len + replacement_len + extra_len;
  if (new_len + 1 > cap){
    return 0;
  }
  /* Using memmove below is ok, but it could have been done with malloc operations too, creating and deleting buffers as 
  we needed. The point in using memmove is because I found it is faster. For what I am building here it is nothing major,
  probably a kind of "too much", but I wanted to try it anyway. Nevertheless, it has not been easy to think about it 
  in the first place.*/
  memmove(buf + token_offset + replacement_len + extra_len, /* We are moving bytes from one address to the other. We are worknig with addreses here */
          buf + token_offset + token_len,
          *len - (token_offset + token_len) + 1);
  memcpy(buf + token_offset, replacement, replacement_len); /* in the space we created above, we are putting the new bytes (the chars). */
  if (appent_extra_char){
    buf[token_offset + replacement_len] = extra_char;
  }
  buf[new_len] = '\0';
  *len = new_len;
  return 1;
}

size_t find_executable_prefix_match( const char *prefix, const char *path_env, char matches[][NAME_MAX + 1], size_t max_matches){
  if (!prefix || !*prefix || !path_env || !matches || max_matches == 0){ return 0; }
  size_t prefix_len = strlen(prefix);
  char *path_copy = strdup(path_env);
  if (!path_copy){
    return 0;
  }
  char *saveptr = NULL;
  char *dir = strtok_r(path_copy, ":", &saveptr);
  size_t match_count = 0;
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
        if (n < 0 || (size_t)n >= sizeof(fullpath)){
          continue;
        }
        if (!is_executable_file(fullpath)){
          continue;
        }
        int duplicate = 0;
        for (int i = 0; i < match_count; i++){
          if (strcmp(matches[i], name) == 0){
            duplicate = 1;
            break;
          }
        }
        if (duplicate){
          continue;
        }
        if (match_count < max_matches){
          strncpy(matches[match_count], name, NAME_MAX);
          matches[match_count][NAME_MAX] = '\0';
          match_count++;
        }
      }
      closedir(dp);
    }
    dir = strtok_r(NULL, ":", &saveptr);
  }
  free(path_copy);
  return match_count;
}

/* Fork redirection process functions */
int prepare_command_redirection(Command *cmd, int *mode, char **path){
  if (!cmd || !cmd->argv || cmd->argc <= 0 || !mode || !path) {
    return 0;
  }
  *mode = 0;
  *path = NULL;
  for (int i = 0; i < cmd->argc; i++){
    if(!cmd->argv[i]){ break; }
    int m = has_print_to_file_command(cmd->argv[i]);
    if (m != 0){
      if (i + 1 >= cmd->argc || cmd->argv[i + 1] == NULL){
        fprintf(stderr, "redirection syntax error\n");
        return -1;
      }
      *mode = m;
      *path = cmd->argv[i+1];
      cmd->argv[i] = NULL;
      cmd->argc = i;
      return 1;
    }
  }
  return 1;
}

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
  switch (mode) { /* Mode values are returned by the function. Those can range from 1 to 5 */
    case 1:
      *target_fd = STDOUT_FILENO;
      break;
    case 2:
      *target_fd = STDERR_FILENO;
      break;
    case 3: /* Case 3 and 4 behave the same */
    case 4:
      *target_fd = STDOUT_FILENO;
      writing_type = O_APPEND;
      break;
    case 5:
      *target_fd = STDERR_FILENO;
      writing_type = O_APPEND;
      break;
    default:
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

int setup_child_redirection(int mode, const char *path) {
  int writing_type = O_TRUNC;
  if (mode == 0) {
      return 1;
  }
  int target_fd;
  switch (mode) { /* Mode values are returned by the function. Those can range from 1 to 5 */
    case 1:
      target_fd = STDOUT_FILENO;
      break;
    case 2:
      target_fd = STDERR_FILENO;
      break;
    case 3: /* Case 3 and 4 behave the same */
    case 4:
      target_fd = STDOUT_FILENO;
      writing_type = O_APPEND;
      break;
    case 5:
      target_fd = STDERR_FILENO;
      writing_type = O_APPEND;
      break;
    default:
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

/* Function for handling the case of completing the command that is inserted by the user */
int handle_tab_command(char *buf, size_t *len, size_t cap, size_t token_offset, size_t token_len, const char **cmds, size_t cmd_count, const char *path_env, int show_all_matches){
  (void)token_offset; /* silencing warnings at compiling time since we are not using it */
  char token[NAME_MAX - 1];
  if (token_len > NAME_MAX){
    return 0;
  }
  memcpy(token, buf, token_len);
  token[token_len] = '\0';
  char builtin_matches[256][NAME_MAX + 1];
  int builtin_count = 0;
  for (int i = 0; i < cmd_count && builtin_count < 256; i++){
    if (strncmp(cmds[i], token, token_len) == 0){
      strncpy(builtin_matches[builtin_count], cmds[i], NAME_MAX);
      builtin_matches[builtin_count][NAME_MAX] = '\0';
      builtin_count++;
    }
  }
  char exec_matches[256][NAME_MAX + 1];
  size_t exec_count = find_executable_prefix_match(token, path_env, exec_matches, 256);
  char (*matches)[NAME_MAX + 1] = NULL;
  size_t match_count = 0;
  if (exec_count > 0){
    matches = exec_matches;
    match_count = exec_count;
  } else {
    matches = builtin_matches;
    match_count = builtin_count;
  }
  if (match_count == 0){
    printf("\a");
    fflush(stdout);
    return 0;
  }
  if (match_count == 1){
    size_t match_len = strlen(matches[0]);
    if (match_len + 2 > cap){
      return 0;
    }
    memcpy(buf, matches[0], match_len);
    buf[match_len] = ' ';
    buf[match_len + 1] = '\0';
    *len = match_len + 1;
    printf("\r$ %s", buf);
    fflush(stdout);
    return 1;
  }
  /* There are multiple matches */
  size_t lcp_len = longest_common_len(matches, match_count);
  if (lcp_len > *len){
    if (lcp_len + 1 > cap){
      return 0;
    }
    memcpy(buf, matches[0], lcp_len);
    buf[lcp_len] = '\0';
    *len = lcp_len;
    printf("\r$ %s", buf);
    fflush(stdout);
    return 1;
  }
  if (!show_all_matches){
    printf("\a");
    fflush(stdout);
    return 0;
  }
  /* Noting that qsort takes the address of the cmp_strings function, equivalent to &cmp_strings
  Functionally this is like doing cmp_strings(&matches[i], &matches[j]) */
  qsort(matches, match_count, sizeof(matches[0]), cmp_strings); /* sorting the list of arrays */
  printf("\n");
  for (size_t i = 0; i < match_count; i++){
    printf("%s", matches[i]);
    if (i + 1 < match_count){
      printf(" ");
    }
  }
  printf("\n$ %s", buf);
  fflush(stdout);
  return 0;
}

/* Function for handling the case of completing the path provided by the user, be for files or directories */
int handle_tab_path(char *buf, size_t *len, size_t cap, size_t token_offset, size_t token_len, int show_all_matches){
  if (!buf || !len){
    printf("\a");
    fflush(stdout);
    return 0;
  }
  char token[PATH_MAX];
  if (token_len + 1 > sizeof(token)){
    printf("\a");
    fflush(stdout);
    return 0;
  }
  memcpy(token, buf + token_offset, token_len);
  token[token_len] = '\0';
  char dir_to_open[PATH_MAX];
  char prefix[NAME_MAX + 1];
  char replacement_base[PATH_MAX];
  if (!split_path_token(token, dir_to_open, sizeof(dir_to_open), prefix, sizeof(prefix), replacement_base, sizeof(replacement_base))){
    printf("\a");
    fflush(stdout);
    return 0;
  }
  PathMatch matches[256];
  size_t match_count = find_path_matches(dir_to_open, prefix, replacement_base, matches, 256);
  if (match_count == 0){
    printf("\a");
    fflush(stdout);
    return 0;
  }
  if (match_count == 1){
    char suffix;
    if (matches[0].is_dir){
      suffix = '/';
    } else {
      suffix = ' ';
    }
    if (!replace_token_in_buffer(buf, len, cap, token_offset, token_len, matches[0].text, 1, suffix)){
      printf("\a");
      fflush(stdout);
      return 0;
    }
    printf("\r$ %s", buf);
    fflush(stdout);
    return 1;
  }
  size_t lcp_len = longest_common_len_path_matches(matches, match_count);
  if (lcp_len > token_len){
    if (!replace_token_in_buffer(buf, len, cap, token_offset, token_len, matches[0].text, 0, '\0')){
      printf("\a");
      fflush(stdout);
      return 0;
    }
    buf[token_offset + lcp_len] = '\0';
    *len = token_offset + lcp_len;
    printf("\r$ %s", buf);
    fflush(stdout);
    return 1;
  }
  if (!show_all_matches){
    printf("\a");
    fflush(stdout);
    return 0;
  }
  qsort(matches, match_count, sizeof(matches[0]), cmp_path_matches);
  printf("\n");
  for (size_t i = 0; i < match_count; i++){
    printf("%s", matches[i].text);
    if(matches[i].is_dir){
      printf("/");
    }
    if (i + 1 < match_count){
      printf(" ");
    }
  }
  printf("\n$ %s", buf);
  fflush(stdout);
  return 0;
}

/* Management of user entry and tab completition */
int handle_tab(char *buf, size_t *len, size_t cap, const char **cmds, size_t cmd_count, const char *path_env, int show_all_matches){
  if (!buf || !len){
    return 0;
  }
  if (*len == 0){
    printf("\a");
    fflush(stdout);
    return 0;
  }
  char *token_start = find_current_token_start(buf, *len);
  if (!token_start){
    return 0;
  }
  size_t token_offset = (size_t)(token_start-buf); /* gives back the index where the current token starts (in terms of positioning) */
  size_t token_len = *len - token_offset;
  if (token_offset == 0){
    if (token_len == 0){
      printf("\a");
      fflush(stdout);
      return 0;
    }
    return handle_tab_command(buf, len, cap, token_offset, token_len, cmds, cmd_count, path_env, show_all_matches);
  }
  return handle_tab_path(buf, len, cap, token_offset, token_len, show_all_matches);
}

int read_command_line(char *buf, size_t cap, const char *path_env, History *h){
  if (buf == NULL || cap == 0){
    return -1;
  }
  size_t cmd_count = sizeof(builtin_cmds) / sizeof(builtin_cmds[0]);
  size_t len = 0;
  size_t cursor = 0;
  buf[0] = '\0';
  int tab_pressed_once = 0;
  char last_tab_buf[cap];
  last_tab_buf[0] = '\0';

  int h_index = (h != NULL) ? h->count : 0;
  int nav_history = 0;
  char draft_buf[cap];
  draft_buf[0] = '\0';

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
      if (cursor > 0){
        memmove(&buf[cursor-1], &buf[cursor], len-cursor+1);
        cursor--;
        len --;
        write(STDOUT_FILENO, "\x1b[D", 3);
        write(STDOUT_FILENO, &buf[cursor], len - cursor);
        write(STDOUT_FILENO, " ", 1);
        size_t move_back = (len - cursor) + 1;
        for (size_t i = 0; i < move_back; i++){
          write(STDOUT_FILENO, "\x1b[D", 3);
        }
      }
      tab_pressed_once = 0;
      last_tab_buf[0] = '\0';
      continue;
    }
    if (c == '\t') {
      int show_all_matches = 0;
      if (tab_pressed_once && strcmp(buf, last_tab_buf) == 0){
        show_all_matches = 1;
      }
      handle_tab(buf, &len, cap, builtin_cmds, cmd_count, path_env, show_all_matches);
      cursor = len;
      if (!show_all_matches){
        strncpy(last_tab_buf, buf, cap-1);
        last_tab_buf[cap-1]= '\0';
        tab_pressed_once = 1;
      } else {
        tab_pressed_once = 0;
        last_tab_buf[0] = '\0';
      }
      continue;
    }
    if (c >= 32 && c < 127){
      if (len + 1 < cap){
        if (cursor == len){
          buf[cursor] = c;
          cursor++;
          len++;
          buf[len] = '\0';
          write(STDOUT_FILENO, &c, 1);
        } else {
          memmove(&buf[cursor + 1], &buf[cursor], len - cursor + 1);
          buf[cursor] = c;
          cursor++;
          len++;
          write(STDOUT_FILENO, &buf[cursor-1], len - (cursor - 1));
          size_t tail = len - cursor;
          for (size_t i = 0; i < tail; i++){
            write(STDOUT_FILENO, "\x1b[D", 3);
          }
        }
      }
      tab_pressed_once = 0;
      last_tab_buf[0] = '\0';
      continue;
    }
    if (c == 27){
      /* 
      The below is because when Arrow UP or Arrow DOWN is used, the combination received is 27 X Y, with 
      X a value '[' and Y either 'A' (ArrowUP) or 'B'(ArrowDOWN)
      */
      char seq[2];
      ssize_t n1 = read(STDIN_FILENO, &seq[0], 1);
      ssize_t n2 = read(STDIN_FILENO, &seq[1], 1);
      if (n1 == 1 && n2 == 1 && seq[0] == '['){
        switch (seq[1])
        {
          case 'A':
            handle_arrow_up(buf, cap, &len, h, &h_index, &nav_history, draft_buf, sizeof(draft_buf), &cursor, "$ ");
            break;
          case 'B':
            handle_arrow_down(buf, cap, &len, h, &h_index, &nav_history, draft_buf, &cursor, "$ ");
            break;
          case 'D':
            handle_arrow_left(&cursor);
            break;
          case 'C':
            handle_arrow_right(&cursor, len);
            break;
          default:
            break;
        }
      }
      tab_pressed_once = 0;
      last_tab_buf[0] = '\0';
      continue;
    }
  }
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
    size_t cap = strlen(input) + 1;
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

/* The below is to free memory taken by the Pipeline */
void free_pipeline(Pipeline *pl){
  if (!pl) return;
  free(pl->cmds);
  pl->cmds = NULL;
  pl->count = 0;
}

/* The below function aims at building the pipeline tracker should the user write one */
int build_pipeline_from_argv(char **argv, int argc, Pipeline *pl){
  if (!argv || argc <= 0 || !pl){
    return -1;
  }
  pl->cmds = NULL;
  pl->count = 0;
  int pipe_count = 0;
  for (size_t i = 0; i < argc; i++){
    if (argv[i] && strcmp(argv[i], "|") == 0){
      pipe_count++;
    }
  }
  int cmd_count = pipe_count + 1;
  Command *cmds = calloc((size_t)cmd_count, sizeof(Command));
  if(!cmds) { return -2; }
  int cmd_index = 0;
  int current_argc = 0;
  int expecting_command = 1;
  for (size_t i = 0; i < argc; i++){
    if (!argv[i]){
      free(cmds);
      return -3;
    }
    if (strcmp(argv[i], "|") == 0 || strcmp(argv[i], "||") == 0){
      if (expecting_command){
        free(cmds);
        return -4; // Here we have a situation like ||
      }
      argv[i] = NULL;
      cmds[cmd_index].argc = current_argc;
      cmd_index++;
      current_argc = 0;
      expecting_command = 1;
      continue;
    }
    if (expecting_command){
      cmds[cmd_index].argv = &argv[i];
      expecting_command = 0;
    }
    current_argc++;
  }
  if (expecting_command){
    free(cmds);
    return -5;
  }
  cmds[cmd_index].argc = current_argc;
  pl->cmds = cmds;
  pl->count = cmd_count;
  return cmd_count;
}

/* 
Moved the logic below out in its own function in order to execute a single command. 
As I have worked on the situations, I realised that this piece of logic will be required 
in the child once pipelines are in place. So I decided to isolate it and keep it unique, 
simlifying debug and "maintenance".
*/
int execute_builtin(Command *cmd, const char *path_env, int path_exist, char *candidate, size_t candidate_size, History *h){
  if (!cmd || !cmd->argv || cmd->argc <= 0 || !cmd->argv[0]) {
    return -1;
  }
  char **argv = cmd->argv;
  int argc = cmd->argc;
  char *command = argv[0];
  if (strcmp(command , "exit") == 0){
    return SHELL_EXIT_REQUESTED;
  }
  if (strcmp(command, "history") == 0){
    if (argc <= 2){
      print_history(h, argv);
      return 0;
    } else {
      fprintf(stderr, "history: too many arguments\n");
      return -1;
    }
  }
  if (strcmp(command, "echo") == 0) {
    for (int i = 1; i < argc; i++) {
      if (i > 1) {
        printf(" ");
      }
      printf("%s", argv[i]);
    }
    printf("\n");
    return 0;
  }
  if (strcmp(command, "pwd") == 0) {
    /*
    Buffer and size automatically managed by UNIX. In this case we stored into cwd for later freeing up memory,
    cause internally getcwd uses malloc. if we do not track it with a pointer, we lose track of memory
    */
    char *cwd = getcwd(NULL, 0);
    if (!cwd) {
      perror("getcwd");
      return -4;
    }
    printf("%s\n", cwd);
    free(cwd);
    return 0;
  }
  if (strcmp(command, "cd") == 0) {
    const char *dest = NULL;
    if (argc >= 2) {
      dest = argv[1];
      if (strcmp(dest, "~") == 0) {
        dest = getenv("HOME");
      }
    } else {
      dest = getenv("HOME");
    }
    if (!dest) {
      fprintf(stderr,"cd: HOME not set\n");
      return -5;
    }
    if (chdir(dest) != 0) {
      fprintf(stderr,"cd: %s: No such file or directory\n", dest);
    }
    return 0;
  }
  if (strcmp(command, "type") == 0) {
    if (argc < 2) {
      printf("No command has been inserted.\n");
      return -6;
    }
    char *q = argv[1];
    if (is_builtin_cmd(q)) {
      printf("%s is a shell builtin\n", q);
    } else if (path_exist && find_in_path(q, path_env, candidate, candidate_size)) {
      printf("%s is %s\n", q, candidate);
    } else {
      printf("%s: not found\n", q);
    }
    return 0;
  }
  return -7;
}

/* This function is shared and will execute commands when there is a fork */
void exec_external_command(Command *cmd, const char *path_env, int path_exist, char *candidate, size_t candidate_size){
  char *command = cmd->argv[0];
  if (path_exist && find_in_path(command, path_env, candidate, candidate_size)){
    execv(candidate, cmd->argv);
    perror("execv did not work out");
    _exit(127);
  }
  fprintf(stderr, "%s: command not found\n", command);
  _exit(127);
}

/* This was the original main() function. It becomes a stand alone method as we moved to pipelines.
With all frankness, it is not the complete main(). Few things have been left there, but hte majority
the logic is now here.    */
int execute_single_command(Command *cmd, const char *path_env, int path_exist, char *candidate, size_t candidate_size, History *h){
  if (!cmd || !cmd->argv || cmd->argc <= 0 || !cmd->argv[0]){
    return -1;
  }
  /* Initial conditions */
  int print_to_file = 0;
  int saved_fd = -1;
  int target_fd = -1;
  int special_command_position = -1;
  char *print_to_file_path = NULL;
  /* If the below is correct, we have the command prepared. if not, then we can leave the function. */
  if (prepare_command_redirection(cmd, &print_to_file, &print_to_file_path) < 0){
    return -2;
  }
  char *command = cmd->argv[0];
  if (is_builtin_cmd(command)){
    if (!setup_redirection(print_to_file, print_to_file_path, &saved_fd, &target_fd)){
      perror("setup_redirection");
      return -3;
    }
    int rc = execute_builtin(cmd, path_env, path_exist, candidate, candidate_size, h);
    restore_redirection(&saved_fd, target_fd);
    return rc;
  }
  /*
  Executing the file. I feel this section should be improved for better error handling and less bespoke code
  */
  pid_t pid = fork();     /* We are getting the child PID */
  if (pid == 0) {         /* We are in the child */
    if (print_to_file) {
      if (!setup_child_redirection(print_to_file, print_to_file_path)) {
        perror("redirection");
        _exit(1);
      }
    }
    exec_external_command(cmd, path_env, path_exist, candidate, candidate_size);
  } else if (pid > 0) {   /* We are in the parent */
    int status;
    waitpid(pid, &status, 0);
    return 0;
  } else {
    perror("Fork did not work out.");
    return -4;
  }
  return -7;
}

/* Function for executing pipelines */
int execute_multi_command(Pipeline *pl, const char *path_env, int path_exist, History *h){
  if (!pl || !pl->cmds || pl->count <= 1){
    return -1;
  }
  int ncmds = pl->count;
  int npipes = ncmds - 1;
  int pipes[npipes][2];
  pid_t pids[ncmds];
  for (int i = 0; i < npipes; i++){
    if (pipe(pipes[i]) == -1){
      perror("pipe");
      return -2;
    }
  }
  for (int i = 0; i < ncmds; i++){
    pid_t pid = fork();
    if (pid < 0){
      perror("fork");
      for (int k = 0; k < npipes; k++){
        close(pipes[k][0]);
        close(pipes[k][1]);
      }
      return -3;
    }
    if (pid == 0){
      char candidate[4096];
      int print_to_file = 0;
      char *print_to_file_path = NULL;
      if (prepare_command_redirection(&pl->cmds[i], &print_to_file, &print_to_file_path) < 0){
        _exit(2);
      }
      if (i > 0){
        if (dup2(pipes[i - 1][0], STDIN_FILENO) == -1){
          perror("dup2 stdin");
          _exit(1);
        }
      }
      if (i < ncmds - 1){
        if (dup2(pipes[i][1], STDOUT_FILENO) == -1){
          perror("dup2 stdout");
          _exit(1);
        }
      }
      for (int k = 0; k < npipes; k++){
        close(pipes[k][0]);
        close(pipes[k][1]);
      }
      if (print_to_file){
        if (!setup_child_redirection(print_to_file, print_to_file_path)) {
          perror("redirection");
          _exit(1);
        }
      }
      if (is_builtin_cmd(pl->cmds[i].argv[0])){
        int rc = execute_builtin(&pl->cmds[i], path_env, path_exist, candidate, sizeof(candidate), h);
        if (rc == SHELL_EXIT_REQUESTED){
          _exit(0);
        } else {
          _exit(rc);
        }
      }
      exec_external_command(&pl->cmds[i], path_env, path_exist, candidate, sizeof(candidate));
      _exit(127);
    }
    pids[i] = pid; /* <--- Parent process continues from here after fork() */
  }
  int status;
  for (int i = 0; i < npipes; i++) {
    close(pipes[i][0]);
    close(pipes[i][1]);
  }
  for (int i = 0; i < ncmds; i++) {
    waitpid(pids[i], &status, 0);
  }
  return status;
}

/* Main Function */
int main(){
  struct termios original_termios;
  char user_input[4096];
  // size_t line_cap = 0; /* Temporary removed caused no longer using dynamic memory allocation (for semplicity) */
  char candidate[4096];
  char *argv[BUFFER];     /* This is for the arguments */
  // setbuf(stdout, NULL);   /* Flush after every print */
  const char *path_env = getenv("PATH");
  /* Below variable is for the history of commands. I could have used a struct
  but I decided to go with an array of strings.   */
  History history;
  history.entries = malloc(HISTORY_CAPACITY * sizeof(char *));
  if (!history.entries){
    perror("malloc");
  }
  history.count = 0;
  int path_exist = (path_env != NULL);
  if (!path_exist){
    printf("PATH does not exist or is corrupted.\n");
  }
  if (enable_raw_mode(&original_termios) == -1) {
    perror("enable_raw_mode");
    return 1;
  }
  while (1){
    Pipeline pl= {0};
    setbuf(stdout, NULL);   /* Flush after every print */
    printf("$ ");
    // The function below allows a full string to be read, including the ending caracter '\n'
    int nread = read_command_line(user_input, sizeof(user_input), path_env, &history);
    if (nread < 0){
      printf("\n");
      break; //<-- #TODO: this might need to be continue
    }
    save_history(&history, user_input);
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
    int ncmds = build_pipeline_from_argv(argv, argc, &pl);
    if (ncmds < 0) {
      fprintf(stderr, "Syntax error near pipe\n");
      free_argv(argv, BUFFER);
      continue;
    }
    int rc = 0;
    if (pl.count == 1){
      rc = execute_single_command(&pl.cmds[0], path_env, path_exist, candidate, sizeof(candidate), &history);
    } else {
      rc = execute_multi_command(&pl, path_env, path_exist, &history);
    }
    free_pipeline(&pl);
    free_argv(argv, BUFFER);
    if (rc == SHELL_EXIT_REQUESTED) break;
  }
  free_history(&history);
  disable_raw_mode(&original_termios);
  return 0;
}
