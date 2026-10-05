output "sender_public_ip" {
  value = aws_instance.node["sender"].public_ip
}

output "receiver_public_ip" {
  value = aws_instance.node["receiver"].public_ip
}

output "receiver_mgmt_ip" {
  value = aws_instance.node["receiver"].private_ip
}

output "data_ips" {
  value = { for k, v in local.nodes : k => v.data_ip }
}

output "data_macs" {
  value = { for k, v in aws_network_interface.data : k => v.mac_address }
}

output "instance_type" {
  value = var.instance_type
}

output "ssh_sender" {
  value = "ssh ubuntu@${aws_instance.node["sender"].public_ip}"
}
