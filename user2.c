
#include "ksocket.h"
#include <sys/time.h>

int main(int argc, char *argv[])
{
    if(argc!=5){fprintf(stderr,"Usage: %s src_ip src_port dst_ip dst_port\n",argv[0]);return 1;}
    const char *sip=argv[1]; int sp=atoi(argv[2]);
    const char *dip=argv[3]; int dp=atoi(argv[4]);

    ksockfd_t sock=k_socket(AF_INET,SOCK_KTP,0);
    if(sock<0){perror("k_socket");return 1;}
    if(k_bind(sock,sip,sp,dip,dp)<0){perror("k_bind");return 1;}
    sleep(2);

    char fname[128];
    snprintf(fname,sizeof(fname),"received_%d.txt",sp);
    FILE *fp=fopen(fname,"wb");
    if(!fp){perror("fopen");return 1;}

    char buf[MSGSZ];
    size_t total=0;

    // struct timeval start_time, end_time;
    // int total_messages = 0;
    // int first_packet = 1;

    for(;;){
        ssize_t n=k_recvfrom(sock,buf,MSGSZ,0,NULL,NULL);
        if(n<0){
            if(errno==ENOMESSAGE){usleep(10000);continue;}
            perror("k_recvfrom");fclose(fp);k_close(sock);return 1;
        }

        // if (first_packet) {
        //     gettimeofday(&start_time, NULL);
        //     first_packet = 0;
        // }

        if(n>=1&&buf[0]=='~'){printf("user2: EOF marker\n");break;}

        /* strip zero padding (text files) */
        ssize_t wl=n;
        while(wl>0&&buf[wl-1]=='\0')wl--;
        if(wl>0){fwrite(buf,1,(size_t)wl,fp);fflush(fp);total+=(size_t)wl;}
        printf("user2: received %zd bytes (total %zu)\n",wl,total);
    }
    // gettimeofday(&end_time, NULL);
    // double elapsed = (end_time.tv_sec - start_time.tv_sec) + 
    //                  (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    // double avg = (total_messages > 0) ? (elapsed / total_messages) : 0;

    // printf("\n========= RECEIVER PERFORMANCE REPORT =========\n");
    // printf("Total Unique Messages : %d\n", total_messages);
    // printf("Total Transfer Time   : %.4f seconds\n", elapsed);
    // printf("Average Time/Message  : %.6f seconds\n", avg);
    // printf("===============================================\n\n");
    
    fclose(fp);
    printf("user2: wrote %zu bytes to %s\n",total,fname);
    k_close(sock);
    return 0;
}
