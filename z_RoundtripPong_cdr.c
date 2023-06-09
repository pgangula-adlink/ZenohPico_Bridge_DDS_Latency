
#include <ctype.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <zenoh-pico.h>
#include <RoundTrip.h>

// CycloneDDS CDR Deserializer
#include <dds/cdr/dds_cdrstream.h>

// CDR Xtypes header {0x00, 0x01} indicates it's Little Endian (CDR_LE representation)
const uint8_t cdr_header[4] = {0x00, 0x01, 0x00, 0x00};
z_owned_publisher_t pub;
const struct dds_cdrstream_allocator dds_cdrstream_default_allocator = {malloc, realloc, free};

void idl_deser(unsigned char *buf, uint32_t sz, void *obj, const dds_topic_descriptor_t *desc)
{
    dds_istream_t is = {.m_buffer = buf, .m_index = 0, .m_size = sz, .m_xcdr_version = DDSI_RTPS_CDR_ENC_VERSION_2};
    dds_stream_read(&is, obj, &dds_cdrstream_default_allocator, desc->m_ops);
}

void data_handler(const z_sample_t *sample, void *arg)
{
    (void)(arg);

    z_owned_str_t keystr = z_keyexpr_to_string(sample->keyexpr);
    printf(">> [Subscriber] Received ('%s' size '%d')\n", z_loan(keystr), (int)sample->payload.len);
    z_drop(z_move(keystr));

    // Approximate amount of memory needed to decode incoming message
    // We do this so we only have to allocate once to map this easier to smaller microcontrollers
    size_t decoded_size_approx = sizeof(RoundTripModule_DataType) + sample->payload.len + 1;

    void *msgData = malloc(decoded_size_approx);
    RoundTripModule_DataType *msg = (RoundTripModule_DataType *)msgData;
    // Deserialize Msg
    idl_deser(((unsigned char *)sample->payload.start + 4), (int)sample->payload.len, msgData, &RoundTripModule_DataType_desc);

     // Setup ostream for serializer
    dds_ostream_t os;
    struct dds_cdrstream_desc desc;

    // Allocate buffer for serialized message
    uint8_t *buf = malloc(decoded_size_approx);

        //usleep(100000); (If needed one can induce delay)
        printf("Putting Data ('%s')...\n", "pong/RoundTrip");

        // Add ROS2 header
        memcpy(buf, cdr_header, sizeof(cdr_header));

        os.m_buffer = buf;
        os.m_index = sizeof(cdr_header); // Offset for CDR Xtypes header
        os.m_size = decoded_size_approx;
        os.m_xcdr_version = DDSI_RTPS_CDR_ENC_VERSION_2;

        // Do serialization
        bool ret = dds_stream_write(&os, &dds_cdrstream_default_allocator,
                                    (void *)msg, RoundTripModule_DataType_desc.m_ops);

        if (ret == true)
        {
            z_publisher_put_options_t options = z_publisher_put_options_default();
            options.encoding = z_encoding(Z_ENCODING_PREFIX_TEXT_PLAIN, NULL);
            z_publisher_put(z_publisher_loan(&pub), (const uint8_t *)buf, os.m_index, &options);
        }
}

int main(int argc, char **argv)
{
    const char *keyexpr = "ping/RoundTrip";
    const char *keyexprPong  = "pong/RoundTrip";
    const char *mode = "client";
    char *locator = NULL;

    int opt;
    while ((opt = getopt(argc, argv, "k:e:m:")) != -1)
    {
        switch (opt)
        {
        case 'k':
            keyexpr = optarg;
            break;
        case 'e':
            locator = optarg;
            break;
        case 'm':
            mode = optarg;
            break;
        case '?':
            if (optopt == 'k' || optopt == 'e' || optopt == 'm')
            {
                fprintf(stderr, "Option -%c requires an argument.\n", optopt);
            }
            else
            {
                fprintf(stderr, "Unknown option `-%c'.\n", optopt);
            }
            return 1;
        default:
            return -1;
        }
    }

    z_owned_config_t config = z_config_default();
    zp_config_insert(z_config_loan(&config), Z_CONFIG_MODE_KEY, z_string_make(mode));
    if (locator != NULL)
    {
        zp_config_insert(z_config_loan(&config), Z_CONFIG_PEER_KEY, z_string_make(locator));
    }

    printf("Opening session...\n");
    z_owned_session_t s = z_open(z_config_move(&config));
    if (!z_session_check(&s))
    {
        printf("Unable to open session!\n");
        return -1;
    }

    // Start read and lease tasks for zenoh-pico
    if (zp_start_read_task(z_session_loan(&s), NULL) < 0 || zp_start_lease_task(z_session_loan(&s), NULL) < 0)
    {
        printf("Unable to start read and lease tasks");
        return -1;
    }
    
    printf("Declaring publisher for '%s'...\n", keyexprPong);
    pub = z_declare_publisher(z_session_loan(&s), z_keyexpr(keyexprPong), NULL);
     if (!z_publisher_check(&pub))
    {
        printf("Unable to declare publisher for key expression!\n");
        return -1;
    }

    z_owned_closure_sample_t callback = z_closure_sample(data_handler, NULL, NULL);
    printf("Declaring Subscriber on '%s'...\n", keyexpr);
    z_owned_subscriber_t sub =
        z_declare_subscriber(z_session_loan(&s), z_keyexpr(keyexpr), z_closure_sample_move(&callback), NULL);
    if (!z_subscriber_check(&sub))
    {
        printf("Unable to declare subscriber.\n");
        return -1;
    }

    printf("Enter 'q' to quit...\n");
    char c = '\0';
    while (c != 'q')
    {
        fflush(stdin);
        scanf("%c", &c);
    }
    

    z_undeclare_subscriber(z_subscriber_move(&sub));
    z_undeclare_publisher(z_publisher_move(&pub));

    // Stop read and lease tasks for zenoh-pico
    zp_stop_read_task(z_session_loan(&s));
    zp_stop_lease_task(z_session_loan(&s));

    z_close(z_session_move(&s));

    return 0;
}
