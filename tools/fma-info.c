#include "fma/client.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s SOCKET\n", argv[0]);
        return 2;
    }
    struct fma_client client;
    if (fma_client_connect(&client, argv[1]) < 0) {
        perror("connect");
        return 1;
    }
    struct fma_capabilities caps;
    if (fma_client_query_capabilities(&client, &caps) < 0) {
        perror("query capabilities");
        fma_client_close(&client);
        return 1;
    }
    printf("protocol: %u.%u\n", FMA_PROTOCOL_MAJOR, FMA_PROTOCOL_MINOR);
    printf("transport: unix-socket control + shared-frame-pool\n");
    printf("decoders:");
    for (uint32_t codec = FMA_CODEC_H264; codec <= FMA_CODEC_AV1; ++codec) {
        if (caps.decoder_mask & FMA_CODEC_BIT(codec))
            printf(" %s", fma_codec_name(codec));
    }
    if (caps.max_width && caps.max_height)
        printf("\nmaximum: %ux%u\n", caps.max_width, caps.max_height);
    else
        printf("\nmaximum: queried when a decoder is configured\n");
    fma_client_close(&client);
    return 0;
}
