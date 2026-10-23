#include "ar_util.h"
#include <fcntl.h>
#include <stdlib.h>
static int fail(const char *p, const char *m) { fprintf(stderr,"%s ",m); ar_path(stderr,p); fputc('\n',stderr); return 1; }
int main(int argc, char **argv) {
 const char *p; int fd; unsigned char b[4096]; ssize_t n; int badtext=0;
 if(argc<2) return ar_usage(argc,"اطبع ملف","");
 if(argc>2) { fprintf(stderr,"اطبع: %s: %s\n",AR_S6,argv[2]); return 1; }
 p=argv[1]; fd=open(p,O_RDONLY); if(fd<0) { int c=ar_errno_class(errno); return fail(p,c==2?AR_S2:AR_S11); }
 while((n=read(fd,b,sizeof b))>0) { ssize_t i; for(i=0;i<n;i++) if(b[i]<0x20 && b[i]!='\n') { badtext=1; break; } if(badtext) break; if(write(STDOUT_FILENO,b,(size_t)n)!=(ssize_t)n){close(fd);return fail(p,AR_S12);} }
 if(n<0) { close(fd); return fail(p,AR_S12); }
 close(fd); if(badtext) { fprintf(stderr,"اطبع: "); ar_path(stderr,p); fprintf(stderr,": %s\n",AR_S13); return 1; } return 0;
}
