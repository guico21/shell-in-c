## Why this project
The goal of this project has been to build a simple shell in C locally, without using any external libraries and **without** using **AI**, but rather leveraging the C language itself and some OS-specific calls. 
The reason for making this project has been to remove some rust I had with the programming language itself, since I have not been using it consistently for many years (it is the second language I learned after BASIC).

The shell is not complete, as at the time of writing the following features are still missing:
1. Brace expansion
2. Background jobs
3. Stopped jobs
4. Exit codes
5. Tilde expansion
6. Foreground jobs
7. Input redirection (the `<` operator)
8. Profiles
9. Aliases

You might wonder, "_What then have you implemented?_". Well, I did what is not in the list above :)

The project reached a point where I can personally say there are enough functionalities to be usable, and enough to start touching key software engineering practices like memory management, buffers, differnet data structure, and multi-function design (on top of changing the structure multiple times).

While I could have architected the program to be more modular and more maintainable, I forced myself to stay in a single file. This forced me to use a global variable for `history`, for example, otherwise it would have been a mess of pointer passing everywhere, and pointers are not that forgiving (as you know.. or if you do not know, well now you know it!). No additional includes beyond the OS and the C standard library have been allowed.

## Features
The user can:
1. Use built-in functions like echo, exit, type, pwd, cd and history
2. Launch any executable file stored in the file system of the machine
3. Build pipelines with the `|` operator
4. Use the `>`, `>>`, `1>>`, `2>>`, `1>` and `2>` operators
5. Import, export and append command history to external files

**_Quick note_** -> If you export the environment variable `HISTFILE` with the command `export HISTFILE` and assign it to a different file of the one already in use (or simply export the variable via a terminal), or make the deafult file that `HISTFILE` already points to an executable (some `chmod` command would be required for that), then you can easily sync your command history from any terminal you have been using in your system with this simple shell. Give it a go, worst case you will lose the history and just start again. Pretty cool anyway!

## Some limitation(s)
On top of the features that have not been implemented, I am aware the code can be improved in terms of data structures, readability, and how functions are written in general. It is definitely possible. Nevertheless, I am not going to do it right now, because I feel it is good enough to work with, and I have no idea when the next feature will be implemented, if at all.

## Dependencies
This project has been built and tested on macOS. There are functions in the program that are POSIX and UNIX compliant, which makes this project not suitable to run on machines supporting Windows OS unless you change those functions and a few types. A dumb copilot can change those for you and you should be ready to go, just make sure it compiles.

To compile the project, you need `clang` or `gcc`. I used `clang`. The command to build it is:
```
clang ./main.c -o main
```
Then to execute it:
```
./main
```
Pretty easy (and boring). Of course, ensure you are with your terminal in the folder where you are running the compiler, otherwise it will not work.

## Folder structure
Pretty easy: go to `/src`. In there you will have the `main.c` file. Everything is in there.

## Ok, but have you used AI?
The project is mostly (oops!) written by me, except for a couple of istances where I could not get bothered to refactor 2 functions with a different variable name. In that case, I used an LLM copilot to quickly change them for me.

I also used an LLM in the same way I would have been using Stackoverflow in the past or a google search (Stackoverflow usage indeed is decreasing to almost zero for those reasons). There has been scenarios where even the LLM could not give me the explnation to wht I was looking for, and in that case, believe it or not, Stackoverflow has been again there to help (in the past.. it helped me a lot. I feel guilty to have been using LLM for the search, tbh!).

I have not used AI to _vibecode_ or to do similar practices. When I used (see above) I always proposed and demanded my structure, the steps and the memory mamangement I needed. At times though, I faced a memory bug and, after spending few hours in sorting it out, I used the LL copilot to speed me up in finding the issue (often a different integer size... then you wonder why Rust might be interesting.. or maybe not).

I would say that 85% has been me, 15% has been AI.

## Funny thought -> 
If you want to test if a code has been written 100% by AI or it has had been written (or deeply reviewed) by a human, just open the code, point your finger to a portion of it and ask "_why you did this? what is happenign here? what issue did you find?_". Putting aside a little bit of time the human will need to get around the whole codebase (assuming he has been working on it), it will be pretty clear if he/she has been deeply involved by listening to the explanations ;)

