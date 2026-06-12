#include <stdio.h>
#include <stdlib.h>

#define MAX_RECORDS 1000000

typedef struct
{
    double latency_ms;
} Record;

Record records[MAX_RECORDS];

int record_count = 0;

/*
 * Compare function for qsort.
 */
int compare_double(
    const void* a,
    const void* b)
{
    double da =
        ((Record*)a)->latency_ms;

    double db =
        ((Record*)b)->latency_ms;

    if(da < db)
    {
        return -1;
    }

    if(da > db)
    {
        return 1;
    }

    return 0;
}

/*
 * Load CSV file.
 */
int load_csv(
    const char* filename)
{
    FILE* file =
        fopen(filename, "r");

    if(file == NULL)
    {
        perror("fopen");
        return 0;
    }

    char line[1024];

    /*
     * Skip header.
     */
    fgets(
        line,
        sizeof(line),
        file
    );

    while(
        fgets(
            line,
            sizeof(line),
            file
        ) != NULL
    )
    {
        int socket_fd;
        double receive_time;
        int client_id;
        int sequence;
        double send_time;
        double latency_ms;

        int parsed =
            sscanf(
                line,
                "%d,%lf,%d,%d,%lf,%lf",
                &socket_fd,
                &receive_time,
                &client_id,
                &sequence,
                &send_time,
                &latency_ms
            );

        if(parsed == 6)
        {
            records[record_count]
                .latency_ms =
                latency_ms;

            record_count++;

            if(record_count >= MAX_RECORDS)
            {
                break;
            }
        }
    }

    fclose(file);

    return record_count;
}

double calculate_average_latency()
{
    if(record_count == 0)
    {
        return 0.0;
    }

    double sum = 0.0;

    for(
        int i = 0;
        i < record_count;
        i++
    )
    {
        sum +=
            records[i]
                .latency_ms;
    }

    return
        sum /
        record_count;
}

double calculate_median_latency()
{
    if(record_count == 0)
    {
        return 0.0;
    }

    if(record_count % 2 == 0)
    {
        int middle =
            record_count / 2;

        return
            (
                records[middle - 1]
                    .latency_ms +
                records[middle]
                    .latency_ms
            ) / 2.0;
    }

    return
        records[
            record_count / 2
        ].latency_ms;
}

double calculate_percentile(
    double percentile)
{
    if(record_count == 0)
    {
        return 0.0;
    }

    int index =
        (int)(
            percentile *
            record_count
        );

    if(index >= record_count)
    {
        index =
            record_count - 1;
    }

    return
        records[index]
            .latency_ms;
}

double calculate_min_latency()
{
    if(record_count == 0)
    {
        return 0.0;
    }

    double min =
        records[0]
            .latency_ms;

    for(
        int i = 1;
        i < record_count;
        i++
    )
    {
        if(
            records[i]
                .latency_ms <
            min
        )
        {
            min =
                records[i]
                    .latency_ms;
        }
    }

    return min;
}

double calculate_max_latency()
{
    if(record_count == 0)
    {
        return 0.0;
    }

    double max =
        records[0]
            .latency_ms;

    for(
        int i = 1;
        i < record_count;
        i++
    )
    {
        if(
            records[i]
                .latency_ms >
            max
        )
        {
            max =
                records[i]
                    .latency_ms;
        }
    }

    return max;
}

int main()
{
    printf(
        "=====================================\n"
    );
    printf(
        "Benchmark Analyzer\n"
    );
    printf(
        "=====================================\n"
    );

    int loaded =
        load_csv(
            "../raw/benchmark_400_clients_iocp.csv"
        );

    qsort(
        records,
        record_count,
        sizeof(Record),
        compare_double
    );

    printf(
        "Loaded records: %d\n",
        loaded
    );

    printf(
        "Mean latency: %.3f ms\n",
        calculate_average_latency()
    );

    printf(
        "Median latency: %.3f ms\n",
        calculate_median_latency()
    );

    printf(
        "P95 latency: %.3f ms\n",
        calculate_percentile(0.95)
    );

    printf(
        "P99 latency: %.3f ms\n",
        calculate_percentile(0.99)
    );

    printf(
        "Minimum latency: %.3f ms\n",
        calculate_min_latency()
    );

    printf(
        "Maximum latency: %.3f ms\n",
        calculate_max_latency()
    );

    return 0;
}