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
        memmove(&cwd[1], &copy[strlen(hdir)], strlen(cwd) - strlen(hdir));
    }

    // +1 for :, +3 for _$_
    char* prompt = (char*)calloc(strlen(uname)+1+strlen(cwd)+3, sizeof(char));
    if (prompt == NULL) {
        fprintf(stderr, "Error allocating prompt string.");
        exit(1);
    }

    // Move uname, :, cwd into prompt
    memmove(prompt, uname, strlen(uname));
    prompt[strlen(uname)] = ':';
    memmove(prompt+strlen(uname)+1, cwd, strlen(cwd));
    // 4 = 3 characters + null-terminator
    strncpy(prompt+strlen(uname)+1+strlen(cwd), " $ ", 4);

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
        printf("%s\n", hist_entry->line);
        i++;
    }
}

int main(int argc, char *argv[])
{
    char* prompt;

    // initialize the history
    using_history();

    while(1) {
        char *line_in = NULL;
        char *mult_in = NULL;

        prompt = generate_prompt();
        if (prompt == NULL) {
            fprintf(stderr, "Error generating prompt.\n");
            return 1;
        }

        // read input
        line_in = readline(prompt);

        // Wait for input
        if (line_in == NULL) {
            // Received EOF => stdin is closed, no reason for terminal
            printf("\n");
            return 0;
        } else if (strlen(line_in) == 0) {
            // Empty string, don't care to do anything with it, reprompt.
            continue;
        }

        // Add multiline functionality
        while (line_in[strlen(line_in)-1] == '\\') {
            // Overwrite the backslash in the previous line
            line_in[strlen(line_in)-1] = '\0';

            mult_in = readline(" > ");
            strcat(line_in, mult_in);
        }

        add_history(line_in);

        // Tokenize input
        char** comms = tokenize(line_in);

        // free from readline
        free(line_in);

        struct command* cmd = parse_to_chained_commands(comms);
        // char **cmds;
        // int idx;

        // while (cmd != NULL) {
        //     cmds = cmd->argv;

        //     idx = 0;
        //     while (cmds[idx] != NULL) {
        //         printf("%s\n",cmds[idx]);
        //         idx += 1;
        //     }
        //     printf("=====\n");
        //     if (cmd->input_fn != NULL) {
        //         printf("%s\n", cmd->input_fn);
        //     }
        //     if (cmd->output_fn != NULL) {
        //         printf("%s\n", cmd->output_fn);
        //     }

        //     cmd = cmd->next;
        //     printf("\n");
        // }
        // if (cmd->next == NULL) 

        printf("forking result: %d\n", fork_and_exec_commands(cmd));

        // TODO: Free statements
    }

    return 0;
}