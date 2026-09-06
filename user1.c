
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
    sleep(2); /* wait for thread R to bind UDP socket */

    FILE *fp=fopen("lorem_100KB.txt","r");
    if(!fp){perror("fopen");return 1;}

    struct sockaddr_in dst;
    memset(&dst,0,sizeof(dst));
    dst.sin_family=AF_INET;
    dst.sin_port=htons((uint16_t)dp);
    dst.sin_addr.s_addr=inet_addr(dip);

    char buf[MSGSZ];
    size_t total=0;

    // struct timeval start_time, end_time;
    // int total_messages = 0;
    // gettimeofday(&start_time, NULL);

    for(;;){
        size_t nr=fread(buf,1,MSGSZ,fp);
        if(nr==0){
            if(feof(fp)){buf[0]='~';nr=1;printf("user1: sending EOF marker\n");}
            else{perror("fread");fclose(fp);k_close(sock);return 1;}
        }
        /* retry until buffer has space */
        for(;;){
            ssize_t s=k_sendto(sock,buf,nr,0,(struct sockaddr*)&dst,sizeof(dst));
            if(s>=0){
                total+=(size_t)s;
                // total_messages++;
                printf("user1: enqueued %zd bytes (total %zu)\n",s,total);
                break;
            }
            if(errno==ENOSPACE){sleep(1);continue;}
            perror("k_sendto");fclose(fp);k_close(sock);return 1;
        }
        if(nr==1&&buf[0]=='~')break;
    }

    // gettimeofday(&end_time, NULL);
    // double elapsed = (end_time.tv_sec - start_time.tv_sec) + 
    //                  (end_time.tv_usec - start_time.tv_usec) / 1000000.0;
    // double avg = (total_messages > 0) ? (elapsed / total_messages) : 0;

    // printf("\n========== SENDER PERFORMANCE REPORT ==========\n");
    // printf("Total Unique Messages : %d\n", total_messages);
    // printf("Total Queueing Time   : %.4f seconds\n", elapsed);
    // printf("Average Time/Message  : %.6f seconds\n", avg);
    // printf("===============================================\n\n");

    fclose(fp);
    printf("user1: done, waiting for ACKs...\n");
    sleep(3*T);

    k_sockinfo *sm = k_shmat();
    if (sm) {
        size_t t_msgs = sm[sock].stat_msgs;
        size_t t_trans = sm[sock].stat_trans;
        double avg = (t_msgs > 0) ? ((double)t_trans / t_msgs) : 0.0;
        
        printf("\n========== SENDER PERFORMANCE REPORT ==========\n");
        printf("Total Unique Messages : %zu\n", t_msgs);
        printf("Total Transmissions   : %zu\n", t_trans);
        printf("Avg Transmissions/Msg : %.4f\n", avg);
        printf("===============================================\n\n");
        k_shmdt(sm);
    }
    k_close(sock);
    return 0;
}
