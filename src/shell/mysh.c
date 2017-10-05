#include <stdlib.h>
#include <pwd.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <string.h>
#include <signal.h>
#include <readline/readline.h>
#include <readline/history.h>

#include "token.h"
#include "consts.h"
#include "com_parser.h"
#include "com_exec.h"
#include "mysh.h"

/*
 * generates the prompt for the shell
 * username:cwd $ 
 *
 * ret: string containing above prompt
 */
char* generate_prompt() {
    // User ID. Function guaranteed to be successful.
    uid_t uid = getuid();

    // Struct containing user information
    struct passwd* pw = getpwuid(uid);
    if (!pw)
    {
        perror("Error in getpwuid");
        return NULL;
    }
    // User name, home directory
    char* uname = pw->pw_name;
    char* hdir = pw->pw_dir;

    // Path length at most PATH_MAX
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("Error in getcwd");
        return NULL;
    }

    // If process in home directory of user, change prompt
    if (strstr(cwd, hdir) != NULL) {
        // Copy to hold cwd
        char copy[strlen(cwd)];
        strcpy(copy, cwd); // No return value for error conditions

        // Replace /home/USER with ~
        cwd[0] = '~';
        strncpy(&cwd[1], &copy[strlen(hdir)], strlen(cwd) - strlen(hdir)+1);
    }

    // +1 for :, +3 for _$_, +1 for null termination, +10 for coloring
    char* prompt = (char*)calloc(strlen(uname)+1+strlen(cwd)+3+(COL_LENGTH*2)+1, 
        sizeof(char));
    if (prompt == NULL) {
        fprintf(stderr, "Error allocating prompt string.");
        exit(1);
    }

    // Move uname, :, cwd into prompt
    strncpy(prompt, PROMPT_COL, COL_LENGTH);
    strncpy(prompt+COL_LENGTH, uname, strlen(uname));
    prompt[COL_LENGTH+strlen(uname)] = ':';
    strncpy(prompt+COL_LENGTH+strlen(uname)+1, cwd, strlen(cwd));
    strncpy(prompt+COL_LENGTH+strlen(uname)+1+strlen(cwd), 
        COL_RESET, COL_LENGTH);
    // 4 = 3 characters + null-terminator
    strncpy(prompt+COL_LENGTH+strlen(uname)+1+strlen(cwd)+COL_LENGTH, " $ ", 4);

    return prompt;
}

/*
 * function to print history to stdout
 */
void print_history() {
    HIST_ENTRY** hist_list = history_list();
    int i = 0;
    while (1) {
        HIST_ENTRY* hist_entry = hist_list[i];
        if (hist_entry == NULL) {
            return;
        }
        // one indexed history commands
        printf("%4d: %s\n", i+1, hist_entry->line);
        i++;
    }
}

/*
 * retrieve history item n and return that string
 */

char* history_n(int n) {
    HIST_ENTRY** hist_list = history_list();
    // one indexed history commands
    HIST_ENTRY* hist_entry = hist_list[n-1];
    if (hist_entry == NULL) {
        return 0;
    }
    return hist_entry->line;
}

int main(int argc, char *argv[])
{
    char* prompt;

    // initialize the history
    using_history();

    while(1) {
        char* full_in = (char*)calloc(MAX_LINE, sizeof(char));
        if (full_in == NULL) {
            fprintf(stderr, "Error allocating input line.\n");
            exit(1);
        }

        // Input buffer
        char *line_in = NULL;

        prompt = generate_prompt();
        if (prompt == NULL) {
            fprintf(stderr, "Error generating prompt.\n");
            return 1;
        }

        // read input
        line_in = readline(prompt);

        free(prompt);

        // Wait for input
        if (line_in == NULL) {
            // Received EOF => stdin is closed, no reason for terminal
            printf("\n");
            return 0;
        } else if (strlen(line_in) == 0) {
            // Empty string, don't care to do anything with it, reprompt.
            continue;
        }

        // Append to full ln
        strcat(full_in, line_in);
        free(line_in);

        // Add multiline functionality
        while (full_in[strlen(full_in)-1] == '\\') {
            // Overwrite the backslash in the previous line
            full_in[strlen(full_in)-1] = '\0';

            line_in = readline(" > ");
            strcat(full_in, line_in);
            free(line_in);
        }

        // Tokenize input
        char** comms = tokenize(full_in);
        int need_to_free = 1;

        if (comms[0] == NULL) {
            continue;
        } else if (comms[0][0] == '!' && comms[1] == 0) {
            // allow for execution of old commands
            // does not allow for modification

            if (is_number(comms[0], 1, strlen(comms[0]))) {
                free(full_in);
                need_to_free = 0;

                int n = atoi(comms[0] + 1);
                full_in = history_n(n);
                printf("%s\n", full_in);
                comms = tokenize(full_in);
            }

        }

        add_history(full_in);

        // free line
        if (need_to_free) {
            free(full_in);
        }

        struct command* cmd = parse_to_chained_commands(comms);

        // TODO: Free everything
        // printf("forking result: %d\n", fork_and_exec_commands(cmd));
        fork_and_exec_commands(cmd);
    }

    return 0;
}