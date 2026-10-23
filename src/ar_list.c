#include "ar_util.h"
#include <dirent.h>
#include <stdlib.h>
static int bad(const char *p) { fprintf(stderr, "اعرض: %s ", AR_S8); ar_path(stderr,p); fprintf(stderr, ": %s\n", AR_S1); return 1; }
int main(int argc, char **argv) {
 const char *p = argc > 2 ? argv[2] : "."; DIR *d; struct dirent *e;
 if (argc > 2 && argv[1][0] == '-') { fprintf(stderr,"اعرض: %s: %s\n",AR_S6,argv[1]); return 1; }
 if (argc > 2 || (argc == 2 && argv[1][0] == '-')) return ar_usage(argc, "اعرض مسار", argc > 1 ? argv[1] : "");
 if (argc == 2) p = argv[1];
 d=opendir(p); if(!d) return bad(p);
 while((e=readdir(d))) if(strcmp(e->d_name,".") && strcmp(e->d_name,"..")) puts(e->d_name);
 closedir(d); return 0;
}
