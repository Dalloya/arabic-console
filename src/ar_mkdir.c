#include "ar_util.h"
#include <stdlib.h>
int main(int argc, char **argv) {
 const char *p;
 if(argc<2) return ar_usage(argc,"أنشئ مسار","");
 if(argc>2) { fprintf(stderr,"أنشئ: %s: %s\n",AR_S6,argv[2]); return 1; }
 p=argv[1]; if(mkdir(p,0755)==0) return 0;
 switch(ar_errno_class(errno)) {
 case 2: fprintf(stderr,"%s ",AR_S9); ar_path(stderr,p); fprintf(stderr,": %s\n",AR_S2); break;
 case 3: case 1: fprintf(stderr,"%s ",AR_S9); ar_path(stderr,p); fprintf(stderr,": %s\n",AR_S1); break;
 case 4: fprintf(stderr,"%s ",AR_S9); ar_path(stderr,p); fprintf(stderr,": %s\n",AR_S4); break;
 }
 return 1;
}
