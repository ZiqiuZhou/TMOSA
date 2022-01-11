#include <unordered_set>

#include "tweet_stream_process.h"

namespace EventTweet::TweetStream {
	TweetStreamProcess::TweetStreamProcess(ConfigFileHandler& config_file_handler) {
		this->time_interval = config_file_handler.GetValue("snapshot_interval", 1);
		auto start_time_str = config_file_handler.GetValue("start_time");
		this->start_time = time_from_string(start_time_str);
	}

	time_duration::sec_type TweetStreamProcess::ToTimeDuration(std::string&& time_str_format) {
		ptime current_time = time_from_string(time_str_format);
		time_duration duration = current_time - start_time;
		return duration.total_seconds();
	}

    bool TweetStreamProcess::ProcessGLOVE(DataParser &json_parser, SnapShot &snapshot, std::string& filename) {
        // group filename for each snapshot
        std::string post_fix = "_" + std::to_string(snapshot.GetIndex());
        std::size_t pos = filename.rfind('.');
        std::string need_embedding_file;
        std::string extension;
        if (pos != std::string::npos) {
            need_embedding_file = filename.substr(0, std::distance(filename.begin(), filename.begin() + pos));
            extension = filename.substr(pos);
        }
        need_embedding_file += (post_fix + extension);
        std::ifstream file_stream(need_embedding_file);
        FileWriterNormal file_writer;
        file_writer.open(need_embedding_file, FileMode::text);
        if (file_stream.peek() != std::ifstream::traits_type::eof()) {
            std::cout << " file path = " << __FILE__ << " function name = " << __FUNCTION__ << " line = " << __LINE__
                      << " Invalid file." << std::endl;
            return false;
        }

        auto& tweet_map = snapshot.GetTweetMap();
        for (auto& id_tweet: snapshot.GetTweetMap()) {
            Tweet& tweet = id_tweet.second;
            std::string json_string;
            json_parser.TweetToJSON(tweet, json_string);
            auto buffer = std::span(json_string.begin(), json_string.end());
            file_writer.write(buffer);
        }
        file_writer.close();
        //std::remove(need_embedding_file.c_str()); // delete file

        if (!file_stream.fail()) {
            std::cout << " file path = " << __FILE__ << " function name = " << __FUNCTION__ << " line = " << __LINE__
                      << " File not empty." << std::endl;
            return false;
        }

        // process file
        return true;
    }

	bool TweetStreamProcess::StreamProcess(FileReader& file_reader, ConfigFileHandler& config_file_handler) {
		DataParser json_parser;

		HistorySequenceSet history_sequence_set(config_file_handler.GetValue("sequence_length", 200));
		Window sliding_window(config_file_handler.GetValue("window_size", 1));
		SnapShot snapshot(current_snapshot_index);

        std::vector<double> space_bounding_box(config_file_handler.GetVector("space_Houston"));
        Space space(space_bounding_box, 1.0);
		int const window_size = sliding_window.GetWindowSize();
		int const history_length = history_sequence_set.GetHistoryLength();

        std::string word_need_embedding = config_file_handler.GetValue("word_need_embedding");
		// iterate all tweets
        for (std::string_view line : linesInFile(std::move(file_reader))) {
			std::string json_tweet = std::string(line);
			Tweet tweet;
			if (!json_parser.TweetParser(tweet, json_tweet)) {
				continue;
			}
			json_tweet.clear();

            if (!tweet.NeedPredictLocation()) {
                Point point(tweet.GetLongitude(), tweet.GetLatitude());
                if (!space.ContainsPoint(point)) {
                    continue;
                }
                Point new_point = space.ReGenerateCoordinates(point);
                tweet.SetLongitude(new_point.longitude);
                tweet.SetLatitude(new_point.latitude);
            }

			std::string timestamp = tweet.GetCreateTime();
			auto duration = ToTimeDuration(std::move(timestamp));
			// process the entire snapshot
			if (time_interval - duration <= 0) {
                std::cout << "process snapshot: " << snapshot.GetIndex() << std::endl;
				// construct history usage
				if (snapshot.GetIndex() < history_length) {
					history_sequence_set.ManipulateWordHistory(snapshot);
				} else {
                    // 1. get bursty words set at snapshot t
                    BurstyWords bursty_word_set;
                    if (!history_sequence_set.Burst(snapshot, bursty_word_set)) {
                        current_snapshot_index++;
                        snapshot.SetIndex(current_snapshot_index);
                        int step = duration / time_interval;
                        start_time += seconds(step * time_interval);
                        continue;
                    }
                    snapshot.SetBurstyWords(std::move(bursty_word_set));

                    // 2. compute tweet similarity and predict location
                    if (GLOVE) {
                        // word embedding using GLOVE
                        if (ProcessGLOVE(json_parser, snapshot, word_need_embedding)) {

                        }
                    }
                    snapshot.GenerateWordIndexMap();
                    TweetSimilarityHandler similarity_handler(snapshot, config_file_handler);
                    similarity_handler.Init()
                                      .GenerateSimMap();
                    TweetLocationPredictor location_predictor(config_file_handler);
                    location_predictor.Predict(similarity_handler);

                    // 3. clustering
                    DBSCAN dbscan(snapshot, similarity_handler, config_file_handler);
                    dbscan.Cluster();
                    auto& points = dbscan.GetPoints();
                    for (auto point: points) {
                        std::cout << std::setprecision(10) << point.longitude << " " << std::setprecision(10) << point.latitude << " " << point.cluster_id << std::endl;
                    }
                }
				// 4. trigger sliding window to slide and switch to next snapshot
				sliding_window.Slide(snapshot);
				snapshot.Reset();

				current_snapshot_index++;
				snapshot.SetIndex(current_snapshot_index);
				int step = duration / time_interval;
				start_time += seconds(step * time_interval);
			}
			snapshot.GenerateUserTweetMap(tweet);
			snapshot.GenerateWordTweetPair(tweet);

            // last step: collect this tweet
            snapshot.CollectTweet(std::move(tweet));
        }

		return true;
	}
}