// Code generated by protoc-gen-go. DO NOT EDIT.
// versions:
// 	protoc-gen-go v1.26.0
// 	protoc        v3.19.0
// source: cloud/blockstore/private/api/protos/tablet.proto

package protos

import (
	protoreflect "google.golang.org/protobuf/reflect/protoreflect"
	protoimpl "google.golang.org/protobuf/runtime/protoimpl"
	reflect "reflect"
	sync "sync"
)

const (
	// Verify that this generated code is sufficiently up-to-date.
	_ = protoimpl.EnforceVersion(20 - protoimpl.MinVersion)
	// Verify that runtime/protoimpl is sufficiently up-to-date.
	_ = protoimpl.EnforceVersion(protoimpl.MaxVersion - 20)
)

type TResetTabletRequest struct {
	state         protoimpl.MessageState
	sizeCache     protoimpl.SizeCache
	unknownFields protoimpl.UnknownFields

	TabletId   uint64 `protobuf:"varint,1,opt,name=TabletId,proto3" json:"TabletId,omitempty"`
	Generation uint32 `protobuf:"varint,2,opt,name=Generation,proto3" json:"Generation,omitempty"`
}

func (x *TResetTabletRequest) Reset() {
	*x = TResetTabletRequest{}
	if protoimpl.UnsafeEnabled {
		mi := &file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[0]
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		ms.StoreMessageInfo(mi)
	}
}

func (x *TResetTabletRequest) String() string {
	return protoimpl.X.MessageStringOf(x)
}

func (*TResetTabletRequest) ProtoMessage() {}

func (x *TResetTabletRequest) ProtoReflect() protoreflect.Message {
	mi := &file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[0]
	if protoimpl.UnsafeEnabled && x != nil {
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		if ms.LoadMessageInfo() == nil {
			ms.StoreMessageInfo(mi)
		}
		return ms
	}
	return mi.MessageOf(x)
}

// Deprecated: Use TResetTabletRequest.ProtoReflect.Descriptor instead.
func (*TResetTabletRequest) Descriptor() ([]byte, []int) {
	return file_cloud_blockstore_private_api_protos_tablet_proto_rawDescGZIP(), []int{0}
}

func (x *TResetTabletRequest) GetTabletId() uint64 {
	if x != nil {
		return x.TabletId
	}
	return 0
}

func (x *TResetTabletRequest) GetGeneration() uint32 {
	if x != nil {
		return x.Generation
	}
	return 0
}

type TResetTabletResponse struct {
	state         protoimpl.MessageState
	sizeCache     protoimpl.SizeCache
	unknownFields protoimpl.UnknownFields

	Status string `protobuf:"bytes,1,opt,name=Status,proto3" json:"Status,omitempty"`
}

func (x *TResetTabletResponse) Reset() {
	*x = TResetTabletResponse{}
	if protoimpl.UnsafeEnabled {
		mi := &file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[1]
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		ms.StoreMessageInfo(mi)
	}
}

func (x *TResetTabletResponse) String() string {
	return protoimpl.X.MessageStringOf(x)
}

func (*TResetTabletResponse) ProtoMessage() {}

func (x *TResetTabletResponse) ProtoReflect() protoreflect.Message {
	mi := &file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[1]
	if protoimpl.UnsafeEnabled && x != nil {
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		if ms.LoadMessageInfo() == nil {
			ms.StoreMessageInfo(mi)
		}
		return ms
	}
	return mi.MessageOf(x)
}

// Deprecated: Use TResetTabletResponse.ProtoReflect.Descriptor instead.
func (*TResetTabletResponse) Descriptor() ([]byte, []int) {
	return file_cloud_blockstore_private_api_protos_tablet_proto_rawDescGZIP(), []int{1}
}

func (x *TResetTabletResponse) GetStatus() string {
	if x != nil {
		return x.Status
	}
	return ""
}

type TKillTabletRequest struct {
	state         protoimpl.MessageState
	sizeCache     protoimpl.SizeCache
	unknownFields protoimpl.UnknownFields

	TabletId uint64 `protobuf:"varint,1,opt,name=TabletId,proto3" json:"TabletId,omitempty"`
}

func (x *TKillTabletRequest) Reset() {
	*x = TKillTabletRequest{}
	if protoimpl.UnsafeEnabled {
		mi := &file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[2]
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		ms.StoreMessageInfo(mi)
	}
}

func (x *TKillTabletRequest) String() string {
	return protoimpl.X.MessageStringOf(x)
}

func (*TKillTabletRequest) ProtoMessage() {}

func (x *TKillTabletRequest) ProtoReflect() protoreflect.Message {
	mi := &file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[2]
	if protoimpl.UnsafeEnabled && x != nil {
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		if ms.LoadMessageInfo() == nil {
			ms.StoreMessageInfo(mi)
		}
		return ms
	}
	return mi.MessageOf(x)
}

// Deprecated: Use TKillTabletRequest.ProtoReflect.Descriptor instead.
func (*TKillTabletRequest) Descriptor() ([]byte, []int) {
	return file_cloud_blockstore_private_api_protos_tablet_proto_rawDescGZIP(), []int{2}
}

func (x *TKillTabletRequest) GetTabletId() uint64 {
	if x != nil {
		return x.TabletId
	}
	return 0
}

type TKillTabletResponse struct {
	state         protoimpl.MessageState
	sizeCache     protoimpl.SizeCache
	unknownFields protoimpl.UnknownFields
}

func (x *TKillTabletResponse) Reset() {
	*x = TKillTabletResponse{}
	if protoimpl.UnsafeEnabled {
		mi := &file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[3]
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		ms.StoreMessageInfo(mi)
	}
}

func (x *TKillTabletResponse) String() string {
	return protoimpl.X.MessageStringOf(x)
}

func (*TKillTabletResponse) ProtoMessage() {}

func (x *TKillTabletResponse) ProtoReflect() protoreflect.Message {
	mi := &file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[3]
	if protoimpl.UnsafeEnabled && x != nil {
		ms := protoimpl.X.MessageStateOf(protoimpl.Pointer(x))
		if ms.LoadMessageInfo() == nil {
			ms.StoreMessageInfo(mi)
		}
		return ms
	}
	return mi.MessageOf(x)
}

// Deprecated: Use TKillTabletResponse.ProtoReflect.Descriptor instead.
func (*TKillTabletResponse) Descriptor() ([]byte, []int) {
	return file_cloud_blockstore_private_api_protos_tablet_proto_rawDescGZIP(), []int{3}
}

var File_cloud_blockstore_private_api_protos_tablet_proto protoreflect.FileDescriptor

var file_cloud_blockstore_private_api_protos_tablet_proto_rawDesc = []byte{
	0x0a, 0x30, 0x63, 0x6c, 0x6f, 0x75, 0x64, 0x2f, 0x62, 0x6c, 0x6f, 0x63, 0x6b, 0x73, 0x74, 0x6f,
	0x72, 0x65, 0x2f, 0x70, 0x72, 0x69, 0x76, 0x61, 0x74, 0x65, 0x2f, 0x61, 0x70, 0x69, 0x2f, 0x70,
	0x72, 0x6f, 0x74, 0x6f, 0x73, 0x2f, 0x74, 0x61, 0x62, 0x6c, 0x65, 0x74, 0x2e, 0x70, 0x72, 0x6f,
	0x74, 0x6f, 0x12, 0x20, 0x4e, 0x43, 0x6c, 0x6f, 0x75, 0x64, 0x2e, 0x4e, 0x42, 0x6c, 0x6f, 0x63,
	0x6b, 0x53, 0x74, 0x6f, 0x72, 0x65, 0x2e, 0x4e, 0x50, 0x72, 0x69, 0x76, 0x61, 0x74, 0x65, 0x50,
	0x72, 0x6f, 0x74, 0x6f, 0x22, 0x51, 0x0a, 0x13, 0x54, 0x52, 0x65, 0x73, 0x65, 0x74, 0x54, 0x61,
	0x62, 0x6c, 0x65, 0x74, 0x52, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74, 0x12, 0x1a, 0x0a, 0x08, 0x54,
	0x61, 0x62, 0x6c, 0x65, 0x74, 0x49, 0x64, 0x18, 0x01, 0x20, 0x01, 0x28, 0x04, 0x52, 0x08, 0x54,
	0x61, 0x62, 0x6c, 0x65, 0x74, 0x49, 0x64, 0x12, 0x1e, 0x0a, 0x0a, 0x47, 0x65, 0x6e, 0x65, 0x72,
	0x61, 0x74, 0x69, 0x6f, 0x6e, 0x18, 0x02, 0x20, 0x01, 0x28, 0x0d, 0x52, 0x0a, 0x47, 0x65, 0x6e,
	0x65, 0x72, 0x61, 0x74, 0x69, 0x6f, 0x6e, 0x22, 0x2e, 0x0a, 0x14, 0x54, 0x52, 0x65, 0x73, 0x65,
	0x74, 0x54, 0x61, 0x62, 0x6c, 0x65, 0x74, 0x52, 0x65, 0x73, 0x70, 0x6f, 0x6e, 0x73, 0x65, 0x12,
	0x16, 0x0a, 0x06, 0x53, 0x74, 0x61, 0x74, 0x75, 0x73, 0x18, 0x01, 0x20, 0x01, 0x28, 0x09, 0x52,
	0x06, 0x53, 0x74, 0x61, 0x74, 0x75, 0x73, 0x22, 0x30, 0x0a, 0x12, 0x54, 0x4b, 0x69, 0x6c, 0x6c,
	0x54, 0x61, 0x62, 0x6c, 0x65, 0x74, 0x52, 0x65, 0x71, 0x75, 0x65, 0x73, 0x74, 0x12, 0x1a, 0x0a,
	0x08, 0x54, 0x61, 0x62, 0x6c, 0x65, 0x74, 0x49, 0x64, 0x18, 0x01, 0x20, 0x01, 0x28, 0x04, 0x52,
	0x08, 0x54, 0x61, 0x62, 0x6c, 0x65, 0x74, 0x49, 0x64, 0x22, 0x15, 0x0a, 0x13, 0x54, 0x4b, 0x69,
	0x6c, 0x6c, 0x54, 0x61, 0x62, 0x6c, 0x65, 0x74, 0x52, 0x65, 0x73, 0x70, 0x6f, 0x6e, 0x73, 0x65,
	0x42, 0x36, 0x5a, 0x34, 0x61, 0x2e, 0x79, 0x61, 0x6e, 0x64, 0x65, 0x78, 0x2d, 0x74, 0x65, 0x61,
	0x6d, 0x2e, 0x72, 0x75, 0x2f, 0x63, 0x6c, 0x6f, 0x75, 0x64, 0x2f, 0x62, 0x6c, 0x6f, 0x63, 0x6b,
	0x73, 0x74, 0x6f, 0x72, 0x65, 0x2f, 0x70, 0x72, 0x69, 0x76, 0x61, 0x74, 0x65, 0x2f, 0x61, 0x70,
	0x69, 0x2f, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x73, 0x62, 0x06, 0x70, 0x72, 0x6f, 0x74, 0x6f, 0x33,
}

var (
	file_cloud_blockstore_private_api_protos_tablet_proto_rawDescOnce sync.Once
	file_cloud_blockstore_private_api_protos_tablet_proto_rawDescData = file_cloud_blockstore_private_api_protos_tablet_proto_rawDesc
)

func file_cloud_blockstore_private_api_protos_tablet_proto_rawDescGZIP() []byte {
	file_cloud_blockstore_private_api_protos_tablet_proto_rawDescOnce.Do(func() {
		file_cloud_blockstore_private_api_protos_tablet_proto_rawDescData = protoimpl.X.CompressGZIP(file_cloud_blockstore_private_api_protos_tablet_proto_rawDescData)
	})
	return file_cloud_blockstore_private_api_protos_tablet_proto_rawDescData
}

var file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes = make([]protoimpl.MessageInfo, 4)
var file_cloud_blockstore_private_api_protos_tablet_proto_goTypes = []interface{}{
	(*TResetTabletRequest)(nil),  // 0: NCloud.NBlockStore.NPrivateProto.TResetTabletRequest
	(*TResetTabletResponse)(nil), // 1: NCloud.NBlockStore.NPrivateProto.TResetTabletResponse
	(*TKillTabletRequest)(nil),   // 2: NCloud.NBlockStore.NPrivateProto.TKillTabletRequest
	(*TKillTabletResponse)(nil),  // 3: NCloud.NBlockStore.NPrivateProto.TKillTabletResponse
}
var file_cloud_blockstore_private_api_protos_tablet_proto_depIdxs = []int32{
	0, // [0:0] is the sub-list for method output_type
	0, // [0:0] is the sub-list for method input_type
	0, // [0:0] is the sub-list for extension type_name
	0, // [0:0] is the sub-list for extension extendee
	0, // [0:0] is the sub-list for field type_name
}

func init() { file_cloud_blockstore_private_api_protos_tablet_proto_init() }
func file_cloud_blockstore_private_api_protos_tablet_proto_init() {
	if File_cloud_blockstore_private_api_protos_tablet_proto != nil {
		return
	}
	if !protoimpl.UnsafeEnabled {
		file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[0].Exporter = func(v interface{}, i int) interface{} {
			switch v := v.(*TResetTabletRequest); i {
			case 0:
				return &v.state
			case 1:
				return &v.sizeCache
			case 2:
				return &v.unknownFields
			default:
				return nil
			}
		}
		file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[1].Exporter = func(v interface{}, i int) interface{} {
			switch v := v.(*TResetTabletResponse); i {
			case 0:
				return &v.state
			case 1:
				return &v.sizeCache
			case 2:
				return &v.unknownFields
			default:
				return nil
			}
		}
		file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[2].Exporter = func(v interface{}, i int) interface{} {
			switch v := v.(*TKillTabletRequest); i {
			case 0:
				return &v.state
			case 1:
				return &v.sizeCache
			case 2:
				return &v.unknownFields
			default:
				return nil
			}
		}
		file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes[3].Exporter = func(v interface{}, i int) interface{} {
			switch v := v.(*TKillTabletResponse); i {
			case 0:
				return &v.state
			case 1:
				return &v.sizeCache
			case 2:
				return &v.unknownFields
			default:
				return nil
			}
		}
	}
	type x struct{}
	out := protoimpl.TypeBuilder{
		File: protoimpl.DescBuilder{
			GoPackagePath: reflect.TypeOf(x{}).PkgPath(),
			RawDescriptor: file_cloud_blockstore_private_api_protos_tablet_proto_rawDesc,
			NumEnums:      0,
			NumMessages:   4,
			NumExtensions: 0,
			NumServices:   0,
		},
		GoTypes:           file_cloud_blockstore_private_api_protos_tablet_proto_goTypes,
		DependencyIndexes: file_cloud_blockstore_private_api_protos_tablet_proto_depIdxs,
		MessageInfos:      file_cloud_blockstore_private_api_protos_tablet_proto_msgTypes,
	}.Build()
	File_cloud_blockstore_private_api_protos_tablet_proto = out.File
	file_cloud_blockstore_private_api_protos_tablet_proto_rawDesc = nil
	file_cloud_blockstore_private_api_protos_tablet_proto_goTypes = nil
	file_cloud_blockstore_private_api_protos_tablet_proto_depIdxs = nil
}
