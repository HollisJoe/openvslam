#include "openvslam/camera/base.h"
#include "openvslam/data/common.h"
#include "openvslam/data/frame.h"
#include "openvslam/data/keyframe.h"
#include "openvslam/data/landmark.h"
#include "openvslam/data/camera_database.h"
#include "openvslam/data/map_database.h"
#include "openvslam/util/converter.h"

#include <spdlog/spdlog.h>

namespace openvslam {
namespace data {

std::mutex map_database::mtx_database_;

map_database::map_database() {
    spdlog::debug("CONSTRUCT: data::map_database");
}

map_database::~map_database() {
    clear();
    spdlog::debug("DESTRUCT: data::map_database");
}

void map_database::add_keyframe(keyframe* keyfrm) {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    keyframes_[keyfrm->id_] = keyfrm;
    if (keyfrm->id_ > max_keyfrm_id_) {
        max_keyfrm_id_ = keyfrm->id_;
    }
}

void map_database::erase_keyframe(keyframe* keyfrm) {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    keyframes_.erase(keyfrm->id_);

    // TODO: 実体を削除
}

void map_database::add_landmark(landmark* lm) {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    landmarks_[lm->id_] = lm;
}

void map_database::erase_landmark(landmark* lm) {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    landmarks_.erase(lm->id_);

    // TODO: 実体を削除
}

void map_database::set_local_landmarks(const std::vector<landmark*>& local_lms) {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    local_landmarks_ = local_lms;
}

std::vector<landmark*> map_database::get_local_landmarks() const {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    return local_landmarks_;
}

std::vector<keyframe*> map_database::get_all_keyframes() const {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    std::vector<keyframe*> keyframes;
    keyframes.reserve(keyframes_.size());
    for (const auto id_keyframe : keyframes_) {
        keyframes.push_back(id_keyframe.second);
    }
    return keyframes;
}

unsigned int map_database::get_num_keyframes() const {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    return keyframes_.size();
}

std::vector<landmark*> map_database::get_all_landmarks() const {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    std::vector<landmark*> landmarks;
    landmarks.reserve(landmarks_.size());
    for (const auto id_landmark : landmarks_) {
        landmarks.push_back(id_landmark.second);
    }
    return landmarks;
}

unsigned int map_database::get_num_landmarks() const {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    return landmarks_.size();
}

unsigned int map_database::get_max_keyframe_id() const {
    std::lock_guard<std::mutex> lock(mtx_map_access_);
    return max_keyfrm_id_;
}

void map_database::clear() {
    std::lock_guard<std::mutex> lock(mtx_map_access_);

    for (auto& lm : landmarks_) {
        delete lm.second;
        lm.second = nullptr;
    }

    for (auto& keyfrm : keyframes_) {
        delete keyfrm.second;
        keyfrm.second = nullptr;
    }

    landmarks_.clear();
    keyframes_.clear();
    max_keyfrm_id_ = 0;
    local_landmarks_.clear();
    origin_keyfrm_ = nullptr;

    frm_stats_.clear();

    spdlog::info("clear map database");
}

void map_database::from_json(camera_database* cam_db, bow_vocabulary* bow_vocab, bow_database* bow_db,
                             const nlohmann::json& json_keyfrms, const nlohmann::json& json_landmarks) {
    std::lock_guard<std::mutex> lock(mtx_map_access_);

    // 1. データベースを全削除する

    for (auto& lm : landmarks_) {
        delete lm.second;
        lm.second = nullptr;
    }

    for (auto& keyfrm : keyframes_) {
        delete keyfrm.second;
        keyfrm.second = nullptr;
    }

    landmarks_.clear();
    keyframes_.clear();
    max_keyfrm_id_ = 0;
    local_landmarks_.clear();
    origin_keyfrm_ = nullptr;

    // 2. キーフレームを登録，ただしこの時点でオブジェクトが存在しないポインタはnullptrにしておく

    spdlog::info("decoding {} keyframes to load", json_keyfrms.size());
    for (const auto& json_id_keyfrm : json_keyfrms.items()) {
        const auto id = std::stoi(json_id_keyfrm.key());
        assert(0 <= id);
        const auto json_keyfrm = json_id_keyfrm.value();

        register_keyframe(cam_db, bow_vocab, bow_db, id, json_keyfrm);
    }

    // 3. 3次元点を登録，ただしこの時点でオブジェクトが存在しないポインタはnullptrにしておく

    spdlog::info("decoding {} landmarks to load", json_landmarks.size());
    for (const auto& json_id_landmark : json_landmarks.items()) {
        const auto id = std::stoi(json_id_landmark.key());
        assert(0 <= id);
        const auto json_landmark = json_id_landmark.value();

        register_landmark(id, json_landmark);
    }

    // 4. グラフ情報を登録

    spdlog::info("registering essential graph");
    for (const auto& json_id_keyfrm : json_keyfrms.items()) {
        const auto id = std::stoi(json_id_keyfrm.key());
        assert(0 <= id);
        const auto json_keyfrm = json_id_keyfrm.value();

        register_graph(id, json_keyfrm);
    }

    // 5. キーフレームと3次元点の対応を登録

    spdlog::info("registering keyframe-landmark association");
    for (const auto& json_id_keyfrm : json_keyfrms.items()) {
        const auto id = std::stoi(json_id_keyfrm.key());
        assert(0 <= id);
        const auto json_keyfrm = json_id_keyfrm.value();

        register_association(id, json_keyfrm);
    }

    // 6. グラフを更新

    spdlog::info("updating covisibility graph");
    for (const auto& json_id_keyfrm : json_keyfrms.items()) {
        const auto id = std::stoi(json_id_keyfrm.key());
        assert(0 <= id);

        assert(keyframes_.count(id));
        auto keyfrm = keyframes_.at(id);

        keyfrm->update_connections();
        keyfrm->update_covisibility_orders();
    }

    // 7. ジオメトリを更新

    spdlog::info("updating landmark geometry");
    for (const auto& json_id_landmark : json_landmarks.items()) {
        const auto id = std::stoi(json_id_landmark.key());
        assert(0 <= id);

        assert(landmarks_.count(id));
        auto lm = landmarks_.at(id);

        lm->update_normal_and_depth();
        lm->compute_descriptor();
    }
}

void map_database::register_keyframe(camera_database* cam_db, bow_vocabulary* bow_vocab, bow_database* bow_db,
                                     const unsigned int id, const nlohmann::json& json_keyfrm) {
    // 2-0. メタ情報
    const auto src_frm_id = json_keyfrm.at("n_keypts").get<unsigned int>();
    const auto timestamp = json_keyfrm.at("ts").get<double>();
    const auto camera_name = json_keyfrm.at("cam").get<std::string>();
    const auto camera = cam_db->get_camera(camera_name);
    const auto depth_thr = json_keyfrm.at("depth_thr").get<float>();

    // 2-1. 姿勢情報
    const Mat33_t rot_cw = convert_json_to_rotation(json_keyfrm.at("rot_cw"));
    const Vec3_t trans_cw = convert_json_to_translation(json_keyfrm.at("trans_cw"));
    const auto cam_pose_cw = util::converter::to_eigen_cam_pose(rot_cw, trans_cw);

    // 2-2. 特徴点情報
    const auto num_keypts = json_keyfrm.at("n_keypts").get<unsigned int>();
    // keypts
    const auto json_keypts = json_keyfrm.at("keypts");
    const auto keypts = convert_json_to_keypoints(json_keypts);
    assert(keypts.size() == num_keypts);
    // undist_keypts
    const auto json_undist_keypts = json_keyfrm.at("undists");
    const auto undist_keypts = convert_json_to_undistorted(json_undist_keypts);
    assert(undist_keypts.size() == num_keypts);
    // bearings
    auto bearings = eigen_alloc_vector<Vec3_t>(num_keypts);
    assert(bearings.size() == num_keypts);
    camera->convert_keypoints_to_bearings(undist_keypts, bearings);
    // stereo_x_right
    const auto stereo_x_right = json_keyfrm.at("x_rights").get<std::vector<float>>();
    assert(stereo_x_right.size() == num_keypts);
    // depths
    const auto depths = json_keyfrm.at("depths").get<std::vector<float>>();
    assert(depths.size() == num_keypts);
    // descriptors
    const auto json_descriptors = json_keyfrm.at("descs");
    const auto descriptors = convert_json_to_descriptors(json_descriptors);
    assert(descriptors.rows == static_cast<int>(num_keypts));

    // 2-3. ORBスケール情報
    const auto num_scale_levels = json_keyfrm.at("n_scale_levels").get<unsigned int>();
    const auto scale_factor = json_keyfrm.at("scale_factor").get<float>();

    // 2-4. オブジェクト構築
    auto keyfrm = new data::keyframe(id, src_frm_id, timestamp, cam_pose_cw, camera, depth_thr,
                                     num_keypts, keypts, undist_keypts, bearings, stereo_x_right, depths, descriptors,
                                     num_scale_levels, scale_factor, bow_vocab, bow_db, this);

    // 2-5. データベースに追加
    assert(!keyframes_.count(id));
    keyframes_[keyfrm->id_] = keyfrm;
    if (keyfrm->id_ > max_keyfrm_id_) {
        max_keyfrm_id_ = keyfrm->id_;
    }
    if (id == 0) {
        origin_keyfrm_ = keyfrm;
    }
}

void map_database::register_landmark(const unsigned int id, const nlohmann::json& json_landmark) {
    const auto first_keyfrm_id = json_landmark.at("1st_keyfrm").get<int>();
    const auto pos_w = Vec3_t(json_landmark.at("pos_w").get<std::vector<Vec3_t::value_type>>().data());
    const auto ref_keyfrm_id = json_landmark.at("ref_keyfrm").get<int>();
    const auto ref_keyfrm = keyframes_.at(ref_keyfrm_id);
    const auto num_visible = json_landmark.at("n_vis").get<unsigned int>();
    const auto num_found = json_landmark.at("n_fnd").get<unsigned int>();

    auto lm = new data::landmark(id, first_keyfrm_id, pos_w, ref_keyfrm,
                                 num_visible, num_found, this);
    assert(!landmarks_.count(id));
    landmarks_[lm->id_] = lm;
}

void map_database::register_graph(const unsigned int id, const nlohmann::json& json_keyfrm) {
    // グラフ情報
    const auto spanning_parent_id = json_keyfrm.at("span_parent").get<int>();
    const auto spanning_children_ids = json_keyfrm.at("span_children").get<std::vector<int>>();
    const auto loop_edge_ids = json_keyfrm.at("loop_edges").get<std::vector<int>>();

    assert(keyframes_.count(id));
    assert(spanning_parent_id == -1 || keyframes_.count(spanning_parent_id));
    keyframes_.at(id)->set_spanning_parent((spanning_parent_id == -1) ? nullptr : keyframes_.at(spanning_parent_id));
    for (const auto spanning_child_id : spanning_children_ids) {
        assert(keyframes_.count(spanning_child_id));
        keyframes_.at(id)->add_spanning_child(keyframes_.at(spanning_child_id));
    }
    for (const auto loop_edge_id : loop_edge_ids) {
        assert(keyframes_.count(loop_edge_id));
        keyframes_.at(id)->add_loop_edge(keyframes_.at(loop_edge_id));
    }
}

void map_database::register_association(const unsigned int keyfrm_id, const nlohmann::json& json_keyfrm) {
    // 特徴点情報
    const auto num_keypts = json_keyfrm.at("n_keypts").get<unsigned int>();
    const auto landmark_ids = json_keyfrm.at("lm_ids").get<std::vector<int>>();
    assert(landmark_ids.size() == num_keypts);

    assert(keyframes_.count(keyfrm_id));
    auto keyfrm = keyframes_.at(keyfrm_id);
    for (unsigned int idx = 0; idx < num_keypts; ++idx) {
        const auto lm_id = landmark_ids.at(idx);
        if (lm_id < 0) {
            continue;
        }
        if (!landmarks_.count(lm_id)) {
            spdlog::warn("landmark {}: not found in the database", lm_id);
            continue;
        }

        auto lm = landmarks_.at(lm_id);
        keyfrm->add_landmark(lm, idx);
        lm->add_observation(keyfrm, idx);
    }
}

void map_database::to_json(nlohmann::json& json_keyfrms, nlohmann::json& json_landmarks) {
    std::lock_guard<std::mutex> lock(mtx_map_access_);

    // 各キーフレームをJSONに変換して保存する
    spdlog::info("encoding {} keyframes to store", keyframes_.size());
    std::map<std::string, nlohmann::json> keyfrms;
    for (const auto id_keyfrm : keyframes_) {
        const auto id = id_keyfrm.first;
        const auto keyfrm = id_keyfrm.second;
        assert(keyfrm);
        assert(id == keyfrm->id_);
        assert(!keyfrm->will_be_erased());
        keyfrm->update_connections();
        assert(!keyfrms.count(std::to_string(id)));
        keyfrms[std::to_string(id)] = keyfrm->to_json();
    }
    json_keyfrms = keyfrms;

    // 各3次元点をJSONに変換して保存する
    spdlog::info("encoding {} landmarks to store", landmarks_.size());
    std::map<std::string, nlohmann::json> landmarks;
    for (const auto id_lm : landmarks_) {
        const auto id = id_lm.first;
        const auto lm = id_lm.second;
        assert(lm);
        assert(id == lm->id_);
        assert(!lm->will_be_erased());
        lm->update_normal_and_depth();
        assert(!landmarks.count(std::to_string(id)));
        landmarks[std::to_string(id)] = lm->to_json();
    }
    json_landmarks = landmarks;
}

} // namespace data
} // namespace openvslam
