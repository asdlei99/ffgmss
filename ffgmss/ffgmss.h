#ifndef __FFGMSS_H__
#define __FFGMSS_H__

void* gmss_init (void);
void  gmss_exit (void *ctxt);
void  gmss_sound(void *ctxt, char *file);
void  gmss_music_play (void *ctxt, char *files[], int num, int idx);
void  gmss_music_pause(void *ctxt);
void  gmss_music_loop (void *ctxt, int loop);

#endif
