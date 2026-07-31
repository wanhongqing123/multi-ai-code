// 项目在渲染层的形状。原本定义在 ProjectPicker 组件里，随该弹窗一起删除后
// 挪到这里——它描述的是 project.list 返回的数据，本来就不该属于某个弹窗。
export interface ProjectInfo {
  id: string
  name: string
  target_repo: string
  dir: string
  created_at: string
  updated_at: string
}
