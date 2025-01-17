#include <seqan3/search/views/kmer_hash.hpp>

#include <raptor/adjust_seed.hpp>
#include <raptor/dna4_traits.hpp>
#include <raptor/search/do_parallel.hpp>
#include <raptor/search/load_index.hpp>
#include <raptor/search/sync_out.hpp>

#include "compute_distance.hpp"
#include "search.hpp"
#include "options.hpp"
#include "sketch.hpp"

void search(smash_options const & options)
{
    auto index = raptor::raptor_index<raptor::index_structure::hibf>{};

    double index_io_time{0.0};
    double reads_io_time{0.0};
    double compute_time{0.0};

    auto cereal_worker = [&]()
    {
        raptor::load_index(index, options.index_file, index_io_time);
    };
    auto cereal_handle = std::async(std::launch::async, cereal_worker);

    std::vector<std::string> filenames{};

    raptor::sync_out synced_out{options.output_file};

    // {
    //     size_t position{};
    //     std::string line{};
    //     for (auto const & filename : options.files)
    //     {
    //         line.clear();
    //         line = '#';
    //         line += std::to_string(position);
    //         line += '\t';
    //         line += filename;
    //         line.back() = '\n';
    //         synced_out << line;
    //         ++position;
    //     }
    // }

    { // write header line
        std::string line{"#filenames"};
        for (auto const & filename : options.files)
        {
            line += filename;
            line += '\t';
        }
        line.back() = '\n';
        synced_out << line;
    }

    auto worker = [&](size_t const start, size_t const end)
    {
        auto counter = [&index]()
        {
            return index.ibf().template counting_agent<uint16_t>();
        }();

        std::string result_string{};

        for (auto && filename : filenames | seqan3::views::slice(start, end))
        {
            result_string.clear();
            result_string += filename;
            result_string += '\t';

            for (auto && rec : seqan3::sequence_file_input{filename})
            {
                auto && sketch = sketch_min_hash(rec.sequence(), options.kmer_size, options.sketch_size);

                auto & result = counter.bulk_count(sketch);

                for (auto && count : result)
                {
                    auto const dist = compute_distance(count);

                    result_string += std::to_string(dist);
                    result_string += '\t';
                }

            }

            result_string.back() = '\n';
            synced_out.write(result_string);
        }
    };

    for (auto && chunked_files : options.files | seqan3::views::chunk((1ULL << 20) * 10))
    {
        filenames.clear();
        auto start = std::chrono::high_resolution_clock::now();
        std::ranges::move(chunked_files, std::back_inserter(filenames));
        auto end = std::chrono::high_resolution_clock::now();
        reads_io_time += std::chrono::duration_cast<std::chrono::duration<double>>(end - start).count();

        cereal_handle.wait();

        raptor::do_parallel(worker, filenames.size(), options.threads, compute_time);
    }

    // GCOVR_EXCL_START
    if (options.write_time)
    {
        std::filesystem::path file_path{options.output_file};
        file_path += ".time";
        std::ofstream file_handle{file_path};
        file_handle << "Index I/O\tReads I/O\tCompute\n";
        file_handle << std::fixed << std::setprecision(2) << index_io_time << '\t' << reads_io_time << '\t'
                    << compute_time;
    }
    // GCOVR_EXCL_STOP
}
